#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>
#include <soc/usb_serial_jtag_struct.h>

#ifdef CROSSPOINT_TOUCH_INT_WAKE
#include <driver/gpio.h>
#endif

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  updateUsbState(millis());
}

void HalGPIO::updateUsbState(const unsigned long now) {
  // SOF-based host-link sampling (see the member comment). A cheap register
  // read, so it runs at its own short cadence on both devices and is never
  // behind the I2C throttle below — a fresh enumeration must cancel light
  // sleep within a poll or two, or the next slice kills the CDC link again.
  if (sofLastSampleMs == 0 || now - sofLastSampleMs >= SOF_SAMPLE_MS) {
    const auto sof = static_cast<uint16_t>(USB_SERIAL_JTAG.fram_num.sof_frame_index);
    usbSofActive = (sof != lastSofFrameIndex);
    lastSofFrameIndex = sof;
    sofLastSampleMs = now;
  }

  // Throttle the X3's I2C-based USB detection; see USB_POLL_X3_MS. First call
  // (usbLastPollMs == 0) always polls so boot state is correct. The combined
  // verdict below is still recomputed every call so a SOF-detected attach is
  // not held back by the throttle window.
  if (usbLastPollMs == 0 || !deviceIsX3() || now - usbLastPollMs >= USB_POLL_X3_MS) {
    usbLastPollMs = now;
    usbElectricalConnected = isUsbElectricalConnected();
  }
  const bool connected = usbSofActive || usbElectricalConnected;
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

void HalGPIO::pollUsbState() {
  // Wait out the SOF sample floor so the comparison sees a real frame delta:
  // two reads inside one USB frame compare equal and read as "no host".
  const unsigned long elapsed = millis() - sofLastSampleMs;
  if (sofLastSampleMs != 0 && elapsed < SOF_SAMPLE_MS) {
    delay(SOF_SAMPLE_MS - elapsed);
  }
  updateUsbState(millis());
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

bool HalGPIO::isDebouncePending() const { return inputMgr.isDebouncePending(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::isHomeKeyDown() const { return inputMgr.isHomeKeyDown(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  // X4 Pro wakes on any power-button press; other boards retain the configured
  // hold-duration verification below.
  if (BoardConfig::isX4Pro() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }
#if defined(FREEINK_DEVICE_M5PAPER) && FREEINK_DEVICE_M5PAPER
  return true;
#endif
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return true;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot.
  const unsigned long calibration = millis();
  const unsigned long calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

bool HalGPIO::isUsbConnected() const {
  // Recent SOF activity means an enumerated host regardless of what the
  // electrical check says (false at boot until update() has sampled twice).
  return usbSofActive || isUsbElectricalConnected();
}

bool HalGPIO::isUsbElectricalConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging. Misses a data-only cable and a full
    // battery — the SOF check in update() covers those.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect < 0) {
    return false;
  }
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}

#ifdef CROSSPOINT_TOUCH_INT_WAKE

namespace {

struct WakePin {
  gpio_num_t pin;
  bool activeLow;
};

constexpr uint8_t MAX_WAKE_PINS = 8;
WakePin wakePins[MAX_WAKE_PINS];
uint8_t wakePinCount = 0;
bool wakePinOverflow = false;
bool wakeUsable = false;

void addWakePin(const int8_t pin, const bool activeLow) {
  if (pin < 0) {
    return;
  }
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    if (wakePins[i].pin == static_cast<gpio_num_t>(pin)) return;  // shared pin (confirm/power)
  }
  if (wakePinCount >= MAX_WAKE_PINS) {
    // Silently dropping a pin would leave a stretched slice deaf to that input
    // for up to its full length. Fail the whole mechanism closed instead; the
    // caller then keeps the stock slice, whose poll cadence sees every button.
    wakePinOverflow = true;
    return;
  }
  wakePins[wakePinCount++] = {static_cast<gpio_num_t>(pin), activeLow};
}

// True while the pin sits at the level it would wake on.
bool wakePinAsserted(const WakePin& p) { return digitalRead(p.pin) == (p.activeLow ? LOW : HIGH); }

}  // namespace

void HalGPIO::beginInputWake() {
  wakePinCount = 0;
  wakePinOverflow = false;
  wakeUsable = false;

  // Button pin modes already belong to InputManager::begin() (INPUT_PULLUP for
  // the nav keys, powerActiveHigh for power) and the touch INT's to the GT911
  // driver; this only reads their polarity.
  const auto& in = BoardConfig::ACTIVE.input;
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    for (const int8_t pin : {in.back, in.confirm, in.left, in.right, in.up, in.down}) {
      addWakePin(pin, true);
    }
  }
  // The power pin is in the table even though lightSleep() arms it on its own:
  // arming is idempotent, and it has to be here for the held-level test — a
  // power button held down must keep the slice short like any other held input.
  addWakePin(in.power, !in.powerActiveHigh);

  const int8_t touchIrq = inputMgr.touchWakeIrqPin();
  addWakePin(touchIrq, inputMgr.touchWakeIrqActiveLow());

  // Every input the loop can act on has to be something this can arm, or a
  // stretched slice would be deaf to the rest of them for up to its full
  // length. Boards that fail that test keep the stock slice:
  //   * XteinkAdcLadder (X4, X3): the nav keys are resistor steps on a single
  //     ADC pin, found by sampling — there is no per-key level to wake on;
  //   * a board button hook (LilyGo T5 S3's user button on its PCA9535): the
  //     key is behind an I2C expander, invisible to a GPIO wake, and the
  //     expander's own INT line is not modeled here;
  //   * a live touch panel with no level-holding INT: a contact would have no
  //     way to signal during a long slice (see FREEINK_GT911_INT_WAKE, which
  //     is what makes touchWakeIrqPin() report a pin at all);
  //   * more wake pins than the table holds (see addWakePin).
  const bool navKeysAreGpio =
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder && !InputManager::hasButtonHook();
  wakeUsable = wakePinCount > 0 && !wakePinOverflow && navKeysAreGpio && (!inputMgr.hasTouch() || touchIrq >= 0);
  LOG_INF("PWR", "Input wake: %u pin(s), touch INT %d, gpioKeys=%d usable=%d", static_cast<unsigned>(wakePinCount),
          static_cast<int>(touchIrq), static_cast<int>(navKeysAreGpio), static_cast<int>(wakeUsable));
  if (!wakeUsable) {
    wakePinCount = 0;
  }
}

bool HalGPIO::inputWakeAvailable() const { return wakeUsable; }

uint8_t HalGPIO::armInputWake(bool& allArmed) const {
  uint8_t armedCount = 0;
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    // Level triggers are the only thing light sleep can wake on: the GPIO edge
    // detector is clock-gated while asleep, so gpio_wakeup_enable() accepts
    // nothing else. A pin already at that level would end the sleep the moment
    // it starts, so leave it out (see the header for why that costs nothing).
    if (wakePinAsserted(wakePins[i])) continue;
    gpio_wakeup_enable(wakePins[i].pin, wakePins[i].activeLow ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    ++armedCount;
  }
  allArmed = armedCount == wakePinCount;
  return armedCount;
}

void HalGPIO::disarmInputWake() const {
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    gpio_wakeup_disable(wakePins[i].pin);
    gpio_set_intr_type(wakePins[i].pin, GPIO_INTR_DISABLE);
  }
}

#endif  // CROSSPOINT_TOUCH_INT_WAKE
