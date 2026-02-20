#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace {
constexpr uint8_t I2C_ADDR_BQ27220 = 0x55;
constexpr uint8_t I2C_ADDR_DS3231 = 0x68;
constexpr uint8_t I2C_ADDR_QMI8658 = 0x6B;
constexpr uint8_t I2C_ADDR_QMI8658_ALT = 0x6A;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

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

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, 0x2C, &soc)) {
    return false;
  }
  if (soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, 0x08, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, 0x00, &sec)) {
    return false;
  }
  const uint8_t secBcd = sec & 0x7F;
  return secBcd <= 0x59;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(20, 0, 400000);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

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

  const X3ProbeResult pass1 = runX3ProbePass();
  delay(2);
  const X3ProbeResult pass2 = runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Inconclusive probe: use sticky cached identity if available.
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "X3 probe inconclusive, using cached device type: %s",
            cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

int readAveragedAdc(uint8_t pin, uint8_t samples = 8) {
  long sum = 0;
  for (uint8_t i = 0; i < samples; ++i) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return static_cast<int>(sum / samples);
}

int readBiasedAdc(uint8_t pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(2);
  const int v = readAveragedAdc(pin, 12);
  pinMode(pin, INPUT);
  return v;
}

}  // namespace

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  // Battery-pin detection (ADC-only): choose the stronger signal.
  const int adc4 = readBiasedAdc(4);
  const int adc0 = readBiasedAdc(BAT_GPIO0);
  _detectAdcValue = adc4;
  _detectAdcValueGpio0 = adc0;

  static constexpr int kPinLeadMargin = 120;
  _batteryPin = (adc4 > adc0 + kPinLeadMargin) ? 4 : BAT_GPIO0;

  _deviceType = detectDeviceTypeWithFingerprint();

  pinMode(_batteryPin, INPUT);
  pinMode(UART0_RXD, INPUT);
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  if (deviceIsX3()) {
    // X3: Direct GPIO read (inputMgr not yet reliable at this point)
    const uint8_t powerPin = InputManager::POWER_BUTTON_PIN;
    if (digitalRead(powerPin) != LOW) {
      startDeepSleep();
    }
    const unsigned long holdStart = millis();
    while (millis() - holdStart < calibratedDuration) {
      if (digitalRead(powerPin) != LOW) {
        startDeepSleep();
      }
      delay(5);
    }
  } else {
    // X4: Use inputMgr with wait window for it to stabilize
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
      } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getHeldTime() < calibratedDuration);
      if (inputMgr.getHeldTime() < calibratedDuration) {
        startDeepSleep();
      }
    } else {
      startDeepSleep();
    }
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3 uses GPIO20 as I2C SDA; it is not a reliable USB detect signal.
    return false;
  }
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if (deviceIsX3()) {
    // X3 wake classification must not depend on GPIO20 level.
    if (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP) {
      return WakeupReason::PowerButton;
    }
    if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN) {
      return WakeupReason::AfterFlash;
    }
    // Cold power-on on X3 is typically user power button.
    if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON) {
      return WakeupReason::PowerButton;
    }
    return WakeupReason::Other;
  }

  const bool usbConnected = isUsbConnected();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
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
