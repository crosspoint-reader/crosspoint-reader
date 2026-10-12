#include <BoardConfig.h>
#include <HalCapabilities.h>
#include <HalGPIO.h>
#include <HalI2cBus.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <driver/usb_serial_jtag.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  // Held across the whole repeated-start transaction: a touch read interleaving
  // between endTransmission(false) and requestFrom() would break it. Runs on the
  // loop task; touch runs on the sampler. No-op on non-touch boards.
  HalI2cBus::Lock i2cLock;
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

// True when this binary targets one of the Xteink C3 variants. A binary holds
// exactly one MCU family — BoardConfig enforces that with an #error — so
// DEFAULT_DEVICE settles the question at compile time.
constexpr bool buildTargetsXteinkC3() {
  return BoardConfig::DEFAULT_DEVICE.board == BoardConfig::Board::XteinkX4 ||
         BoardConfig::DEFAULT_DEVICE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::DEFAULT_DEVICE.board == BoardConfig::Board::XteinkX3Uc8279;
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
  // Both probes below poke C3 hardware — the X3/X4 fingerprint reads C3 pins and
  // applyXteinkDisplayController() drives the Xteink display bus — so they must
  // not run on another board. BoardConfig::ACTIVE would survive them intact
  // (selectDevice() returns false for a board this binary was not built with,
  // leaving the profile the build's own FREEINK_DEVICE_* flag chose), but the
  // probes themselves would still touch pins that mean something else there.
  if constexpr (buildTargetsXteinkC3()) {
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
  }

  // All board-generic now: inputMgr.begin() already read BoardConfig::ACTIVE.input,
  // and the bus/pin setup below reads the active profile rather than the C3
  // Xteink macros in HalGPIO.h. Verified pin-for-pin against the X3/X4 profiles
  // before converting, so the C3 drives exactly the pins it always did:
  //   EPD_SCLK 8 = display.sclk, EPD_MOSI 10 = display.mosi, EPD_CS 21 =
  //   display.cs, SPI_MISO 7 = sd.miso, BAT_GPIO0 0 = batteryAdc,
  //   UART0_RXD 20 = usbDetect.
  inputMgr.begin();

  // Pre-claim the shared SPI bus with the C3's display + SD pins.
  //
  // C3 ONLY, and the reason is subtle: SPIClass::begin() early-returns once the
  // bus is started, so whoever calls first wins the pin assignment. On the C3
  // the panel and the card share one bus and nothing else claims it, so this
  // pre-claim is correct and load-bearing. On every other board someone better
  // informed gets there first -- the SDK's display driver, SDCardManager, or a
  // board-support layer (the T5S3's BoardT5S3::begin(), which also deselects the
  // LoRa radio sharing that bus before starting it). Pre-claiming here would
  // stick on the wrong pins and undo those deselects.
  //
  // Pins come from the active profile rather than the EPD_* macros; verified
  // equal on the C3 (display.sclk 8 / mosi 10 / cs 21, sd.miso 7).
#if FREEINK_MCU_C3
  const BoardConfig::DisplayPins& display = BoardConfig::ACTIVE.display;
  SPI.begin(display.sclk, BoardConfig::ACTIVE.sd.miso, display.mosi, BoardConfig::ACTIVE.sd.cs);
#endif

  // ADC battery sense. Gated on the board actually reading its battery that way:
  // on a fuel-gauge board the same GPIO is the gauge's I2C bus (on X3, BAT_GPIO0
  // is X3_I2C_SCL), so configuring it as an ADC input would break the bus. This
  // replaces a deviceIsX4() test that would have been TRUE on X4 Pro — whose
  // batteryAdc is PIN_UNASSIGNED — and driven pinMode(-1).
  if (HalCapabilities::hasAdcBattery()) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }

  // Route the capacitive home key to the nav buttons this board physically
  // lacks: tap -> CONFIRM, hold -> BACK. Each role is resolved separately rather
  // than from HalCapabilities::hasBackAndConfirmButtons(), which is false when
  // EITHER pin is missing: a board with a real Back pin but no Confirm pin must
  // keep its real Back and gain only the synthetic Confirm. A board with no home
  // key has nothing to route at all. Evaluated once here, after the board profile
  // is final (including any runtime corrections).
  if (BoardConfig::hasHomeKey()) {
    homeKeyDrivesConfirm_ = BoardConfig::ACTIVE.input.confirm == BoardConfig::PIN_UNASSIGNED;
    homeKeyDrivesBack_ = BoardConfig::ACTIVE.input.back == BoardConfig::PIN_UNASSIGNED;
  }
  if (homeKeyDrivesConfirm_ || homeKeyDrivesBack_) {
    LOG_INF("HW", "Capacitive home key routed: tap->%s hold->%s", homeKeyDrivesConfirm_ ? "CONFIRM" : "(has pin)",
            homeKeyDrivesBack_ ? "BACK" : "(has pin)");
  }

  // USB-presence detect. The pin must exist AND not be shared with the fuel-gauge
  // I2C bus: X3 lists usbDetect = GPIO20, which is also its gauge SDA, so it must
  // stay an I2C line. X4 has no gauge and owns GPIO20 outright; X4 Pro leaves
  // usbDetect unassigned because the pin has not been identified yet.
  const int8_t usbDetect = BoardConfig::ACTIVE.usbDetect;
  const BoardConfig::BatteryGaugeConfig& gauge = BoardConfig::ACTIVE.batteryGauge;
  if (usbDetect != BoardConfig::PIN_UNASSIGNED && usbDetect != gauge.i2cSda && usbDetect != gauge.i2cScl) {
    pinMode(usbDetect, INPUT);
  }
}

// Ported from crosspoint-reader PR #2998 ("fix: Guard GPIO13 power control for
// Xteink C3 boards only", Justin Mitchell / @itsthisjustin). The predicate is
// his, verbatim; the call sites differ because this fork has no light sleep.
bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

// Push one debounced edge into the loop-drained FIFO. Caller holds inputMux_.
// Drops the newest edge if the ring is full — a full ring means the loop task
// has been blocked through 32 distinct transitions, far more than any real burst.
void HalGPIO::pushEdgeLocked(uint8_t button, bool pressed, uint32_t timeMs) {
  const int next = (edgeTail_ + 1) % EDGE_BUF;
  if (next == edgeHead_) {
    return;
  }
  edgeBuf_[edgeTail_] = {button, pressed, timeMs};
  edgeTail_ = next;
}

// Reads the SDK's one-shot multi-touch flags and queues whatever it classified.
// Runs on the sampler task, outside inputMux_ — the SDK reads must not happen
// with interrupts disabled — and takes the lock only to push. Drops the newest
// gesture when the ring is full, matching pushEdgeLocked's policy.
void HalGPIO::latchTouchGestures(const uint32_t timeMs) {
  const auto push = [&](const TouchGesture::Kind kind, const float magnitude, const float nx, const float ny) {
    portENTER_CRITICAL(&inputMux_);
    const int next = (gestureTail_ + 1) % GESTURE_BUF;
    if (next != gestureHead_) {
      gestureBuf_[gestureTail_] = {kind, magnitude, nx, ny, timeMs};
      gestureTail_ = next;
    }
    portEXIT_CRITICAL(&inputMux_);
  };

  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long durationMs = 0;
  float degrees = 0.0f;
  if (inputMgr.wasMultiTouchRotation(degrees, nx, ny, durationMs)) {
    push(TouchGesture::Kind::Rotate, degrees, nx, ny);
  }
  float scale = 1.0f;
  if (inputMgr.wasMultiTouchPinch(scale, nx, ny, durationMs)) {
    push(TouchGesture::Kind::Pinch, scale, nx, ny);
  }
}

// Latches the SDK's one-shot single-contact events. See TouchEvent for why they
// cannot simply be read from the loop task. Drops the newest on a full ring,
// matching pushEdgeLocked.
void HalGPIO::latchTouchEvents() {
  const auto push = [&](const TouchEvent& event) {
    portENTER_CRITICAL(&inputMux_);
    const int next = (touchEventTail_ + 1) % TOUCH_EVENT_BUF;
    if (next != touchEventHead_) {
      touchEventBuf_[touchEventTail_] = event;
      touchEventTail_ = next;
    }
    portEXIT_CRITICAL(&inputMux_);
  };

  const uint32_t now = millis();
  TouchEvent event;
  event.atMs = now;
  // Long press first: it fires WHILE the finger is down, so it precedes anything
  // the same contact produces on release. heldMs stays 0 — the SDK's latched
  // duration belongs to the previous contact until this one ends.
  if (inputMgr.wasTouchLongPress(event.nx, event.ny)) {
    event.kind = TouchEvent::Kind::LongPress;
    push(event);
  }

  // Tap and swipe are mutually exclusive by construction: a tap needs the travel
  // at or under TOUCH_TAP_RELEASE_SLOP_PX and a swipe needs it at or over
  // TOUCH_SWIPE_MIN_PX, and the former is defined as the latter minus one. Both
  // are still asked independently rather than as an if/else, so this reports
  // exactly what the SDK classified rather than a rule restated here.
  const auto heldMs = static_cast<uint16_t>(inputMgr.lastTouchHeldMs());
  TouchEvent swipe;
  if (inputMgr.wasSwipe(swipe.nx, swipe.ny, swipe.nxEnd, swipe.nyEnd)) {
    swipe.kind = TouchEvent::Kind::Swipe;
    swipe.heldMs = heldMs;
    swipe.atMs = now;
    push(swipe);
  }
  TouchEvent tap;
  if (inputMgr.wasTouchTap(tap.nx, tap.ny)) {
    tap.kind = TouchEvent::Kind::Tap;
    tap.heldMs = heldMs;
    tap.atMs = now;
    push(tap);
  }

  if (inputMgr.wasTouchReleased()) {
    portENTER_CRITICAL(&inputMux_);
    touchReleasedPending_ = true;
    portEXIT_CRITICAL(&inputMux_);
  }
}

// One sampling pass: read + debounce the buttons, then latch any edges. Runs on
// the sampler task once started; also called synchronously from update() before
// the sampler is up. inputMgr.update() does the ADC read and must run OUTSIDE the
// critical section (analogRead may take the ADC driver mutex). Only the latching
// of the results into the shared accumulators/queue is done under inputMux_.
#if defined(BUTTON_TRACE) && BUTTON_TRACE
// Names for the raw BTN_* indices, for the bring-up trace below. Raw hardware
// indices, NOT the user's remapped logical roles — the point is to see what the
// hardware reported before any mapping.
static const char* buttonTraceName(uint8_t idx) {
  switch (idx) {
    case HalGPIO::BTN_BACK:
      return "BACK";
    case HalGPIO::BTN_CONFIRM:
      return "CONFIRM";
    case HalGPIO::BTN_LEFT:
      return "LEFT";
    case HalGPIO::BTN_RIGHT:
      return "RIGHT";
    case HalGPIO::BTN_UP:
      return "UP";
    case HalGPIO::BTN_DOWN:
      return "DOWN";
    case HalGPIO::BTN_POWER:
      return "POWER";
    default:
      return "?";
  }
}
#endif

void HalGPIO::sampleOnce() {
  {
    // On a touch board inputMgr.update() runs serviceTouch(), i.e. a GT911 I2C
    // transaction, and this runs on the btnsample task while HalClock and the
    // fuel gauge drive the same bus from the loop task. Serialize them (see
    // docs/contributing/touch-architecture.md, "I2C bus and the input sampler").
    // No-op on non-touch boards.
    //
    // Scoped so the lock is released before the critical section below: the
    // I2C mutex must never be held inside portENTER_CRITICAL(&inputMux_), which
    // disables interrupts and cannot block on a semaphore.
    HalI2cBus::Lock i2cLock;
    inputMgr.update();
  }

  uint8_t live = 0;
  uint8_t pressed = 0;
  uint8_t released = 0;
  for (uint8_t i = 0; i <= BTN_POWER; i++) {
    if (inputMgr.isPressed(i)) live |= (1u << i);
    if (inputMgr.wasPressed(i)) pressed |= (1u << i);
    if (inputMgr.wasReleased(i)) released |= (1u << i);
  }
  const uint32_t now = millis();
  const unsigned long held = inputMgr.getHeldTime();

  // Before the critical section: these read the SDK, and the queue push inside
  // takes inputMux_ itself. Inert on non-touch boards.
  latchTouchGestures(now);
  latchTouchEvents();

  // Capacitive home key -> synthetic CONFIRM button.
  //
  // Boards with a GT911 home key (T5S3, X4 Pro) leave input.confirm unassigned:
  // their whole nav cluster is PIN_UNASSIGNED and confirm was meant to come from
  // touch. That leaves the home key as the only spare input on a board that has
  // barely any, so feed it in as BTN_CONFIRM rather than inventing a new logical
  // button.
  //
  // Injected here, into the level/edge accumulators BEFORE they are latched,
  // rather than as a bare edge-queue push. The firmware has two input paths and
  // a key that drives only one of them is half-dead:
  //   - the edge queue, which ButtonEventManager turns into press types
  //     (FileBrowserActivity, SliderPickerActivity, BmpViewerActivity, ...)
  //   - the wasPressed()/isPressed() bitmask, polled directly by ~15 activities
  //     including MenuListActivity, i.e. the entire settings tree
  // Setting the bits here feeds both: the loop below derives the edges from
  // pressed/released for free, exactly as it does for a real button.
  //
  // One key, two roles: TAP -> CONFIRM, HOLD -> BACK. The board has no Back
  // button at all, so without this an activity cannot be exited -- the single
  // worst usability gap on it.
  //
  // This mirrors the SDK's own InputManager::updateConfirmBackHold() (the
  // InputStyle::DigitalConfirmBackHold layout, used by the M5 PaperColor):
  // a hold asserts BACK, and CONFIRM is emitted on release ONLY if the hold did
  // not happen. That layout cannot simply be selected here -- it drives
  // BoardConfig::ACTIVE.input.confirm, a real GPIO this board does not have,
  // and it runs inside InputManager, below the point where our synthetic key
  // enters -- but its semantics are the right ones and are reproduced exactly.
  //
  // Both roles are emitted as a complete press+release in a single pass. That is
  // deliberate: InputManager gives us discrete one-shot events, not a level, and
  // "the gesture finished" is the only moment we can identify with certainty.
  // A Back click and a Confirm click are what activities consume, so a momentary
  // pair is sufficient and avoids inventing a hold the hardware never reports.
  //
  // Event vocabulary, given InputManager keeps touchHomeKeyDown private:
  //   wasHomeKeyPressed()     -> gesture started (role still unknown)
  //   wasHomeKeyTapped()      -> short release  => CONFIRM
  //   wasHomeKeyLongPressed() -> fires WHILE still held, ~700 ms, and no tap
  //                              follows it                => BACK
  // Because the long event arrives mid-hold and is never followed by a tap, BACK
  // is emitted at that instant and the gesture is then closed out; a stray tap
  // afterwards finds no open press and is ignored.
  if (homeKeyDrivesConfirm_ || homeKeyDrivesBack_) {
    if (inputMgr.wasHomeKeyPressed()) {
      homeKeyHeld_ = true;
    }
    if (homeKeyHeld_ && inputMgr.wasHomeKeyLongPressed()) {
      homeKeyHeld_ = false;
      if (homeKeyDrivesBack_) {
        pressed |= (1u << BTN_BACK);
        released |= (1u << BTN_BACK);
      }
    }
    if (homeKeyHeld_ && inputMgr.wasHomeKeyTapped()) {
      homeKeyHeld_ = false;
      if (homeKeyDrivesConfirm_) {
        pressed |= (1u << BTN_CONFIRM);
        released |= (1u << BTN_CONFIRM);
      }
    }
  }

  portENTER_CRITICAL(&inputMux_);
  liveState_ = live;
  accumPressed_ |= pressed;
  accumReleased_ |= released;
  heldTimeSnapshot_ = held;
  for (uint8_t i = 0; i <= BTN_POWER; i++) {
    // A debounced button makes at most one transition per pass, so a button can
    // appear in pressed or released here, never both.
    if (pressed & (1u << i)) pushEdgeLocked(i, true, now);
    if (released & (1u << i)) pushEdgeLocked(i, false, now);
  }
  portEXIT_CRITICAL(&inputMux_);

#if defined(BUTTON_TRACE) && BUTTON_TRACE
  // Raw touch trace, same purpose as the button trace below and the same
  // reasoning about placement. Uses isTouchHeldAt(), which is const and does NOT
  // consume: the tap/long-press events are one-shot and belong to their real
  // consumers, so observing them here would steal them.
  //
  // This answers the first bring-up question -- is the panel reporting contact at
  // all -- separately from the second, whether anything acts on it. Logged on
  // transition only, so a resting finger does not flood the log.
  if (BoardConfig::hasTouch()) {
    // What the SDK made of the contact, reported from the SAMPLER task at the
    // release edge — i.e. where the one-shot flags are still fresh. That is the
    // point of logging it here rather than in the gesture layer: comparing this
    // line with the [GEST] line the loop prints separates "the SDK never
    // classified it" from "the loop was busy and missed it", which are the two
    // failure modes and want completely different fixes.
    //
    // All three reads are const peeks at latched state, so tracing cannot steal
    // the event from its real consumer.
    if (inputMgr.wasTouchReleased()) {
      float sxn = 0.0f, syn = 0.0f, exn = 0.0f, eyn = 0.0f;
      const bool wasTap = inputMgr.wasTouchTap(sxn, syn);
      const bool wasFlick = inputMgr.wasSwipe(sxn, syn, exn, eyn);
      // Travel measured from the positions this trace sampled itself, in the
      // panel's own pixels, because that is the frame the SDK's 60 px swipe
      // threshold is stated in. Its own endpoints are only filled in once it has
      // decided the contact WAS a swipe, which is no help when the question is
      // why it decided otherwise. A contact reporting tap=0 swipe=0 has almost
      // always moved past the 28 px tap slop but not as far as 60.
      const int travelX =
          static_cast<int>(fabsf(touchTraceLastNx_ - touchTraceDownNx_) * BoardConfig::ACTIVE.displayWidth);
      const int travelY =
          static_cast<int>(fabsf(touchTraceLastNy_ - touchTraceDownNy_) * BoardConfig::ACTIVE.displayHeight);
      LOG_INF("TCH", "release: held=%lums tap=%d swipe=%d travel=%d,%d px (tap slop 28, swipe 60, swipe max 700ms)",
              inputMgr.lastTouchHeldMs(), wasTap ? 1 : 0, wasFlick ? 1 : 0, travelX, travelY);
    }
    float tnx = 0.0f, tny = 0.0f;
    const bool held = inputMgr.isTouchHeldAt(tnx, tny);
    if (held) {
      touchTraceLastNx_ = tnx;
      touchTraceLastNy_ = tny;
      if (!touchTraceWasHeld_) {
        touchTraceDownNx_ = tnx;
        touchTraceDownNy_ = tny;
      }
    }
    if (held != touchTraceWasHeld_) {
      touchTraceWasHeld_ = held;
      if (held) {
        LOG_INF("TCH", "contact DOWN at nx=%.3f ny=%.3f t=%lums", tnx, tny, static_cast<unsigned long>(now));
      } else {
        LOG_INF("TCH", "contact UP t=%lums", static_cast<unsigned long>(now));
      }
    }
  }

  // Raw button trace for board bring-up. Deliberately placed AFTER the critical
  // section: logging inside portENTER_CRITICAL would run with interrupts
  // disabled and can trip the interrupt watchdog, which is exactly the failure
  // this kind of tracing is meant to help diagnose.
  //
  // This is the LOWEST level our firmware sees — straight off InputManager,
  // before ButtonEventManager's press-type FSM and before any activity. So a
  // press that appears here but produces no UI response is being consumed
  // higher up, while a press that never appears here never reached us at all.
  // That distinction is the whole point.
  //
  // Covers every source InputManager reports, including buttons behind an I2C
  // expander via setButtonHook() (the T5S3's user button arrives as BTN_DOWN),
  // which the SDK's own readButtonAdc() diagnostic cannot see — that one is
  // specific to the Xteink ADC ladder and reports raw = -1 elsewhere.
  // GT911 capacitive home key. Traced separately because it is NOT a BTN_* index
  // -- InputManager surfaces it through wasHomeKeyPressed()/Tapped()/LongPressed()
  // -- and because the SDK reads its status bit (0x10) UNCONDITIONALLY, without
  // consulting BoardConfig's touch.hasHomeKey. So detection works on any board
  // with a GT911 even when the profile claims no key, and this trace can settle
  // by experiment whether a given board actually has one.
  if (inputMgr.wasHomeKeyPressed() || inputMgr.wasHomeKeyTapped() || inputMgr.wasHomeKeyLongPressed()) {
    LOG_INF("BTN", "HOMEKEY press=%d tap=%d long=%d | hasHomeKey=%d confirmPin=%d route=%d held=%d t=%lums",
            inputMgr.wasHomeKeyPressed() ? 1 : 0, inputMgr.wasHomeKeyTapped() ? 1 : 0,
            inputMgr.wasHomeKeyLongPressed() ? 1 : 0, BoardConfig::hasHomeKey() ? 1 : 0,
            static_cast<int>(BoardConfig::ACTIVE.input.confirm), homeKeyDrivesConfirm_ ? 1 : 0, homeKeyHeld_ ? 1 : 0,
            static_cast<unsigned long>(now));
  }

  if (pressed != 0 || released != 0) {
    for (uint8_t i = 0; i <= BTN_POWER; i++) {
      const bool down = (pressed & (1u << i)) != 0;
      const bool up = (released & (1u << i)) != 0;
      if (!down && !up) continue;
      LOG_INF("BTN", "%s idx=%u (%s) live=0x%02X t=%lums", down ? "DOWN  " : "UP    ", i, buttonTraceName(i), live,
              static_cast<unsigned long>(now));
    }
  }
#endif
}

void HalGPIO::samplerTask(void* arg) {
  HalGPIO* self = static_cast<HalGPIO*>(arg);
  TickType_t last = xTaskGetTickCount();
  while (self->samplerRunning_.load(std::memory_order_acquire)) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    self->sampleOnce();
  }
  xSemaphoreGive(self->samplerStoppedSemaphore_);
  vTaskDelete(nullptr);  // self-terminate once stopInputSampler() clears the flag
}

void HalGPIO::startInputSampler() {
  if (samplerRunning_.load(std::memory_order_acquire)) {
    return;
  }
  samplerStoppedSemaphore_ = xSemaphoreCreateBinary();
  if (samplerStoppedSemaphore_ == nullptr) {
    LOG_ERR("BTN", "Failed to create input sampler stop semaphore");
    return;
  }
  samplerRunning_.store(true, std::memory_order_release);
  sampleOnce();  // prime so the first loop iteration sees current state
  // Priority above the Arduino loop task (1) so the 10ms cadence holds even while
  // the loop task is busy in a long build slice.
  //
  // Stack: 2 KB on a button-only board — the deepest path there
  // (inputMgr.update → analogRead) was measured at ~380 bytes peak on device, so
  // that leaves >4x headroom while staying small (this codebase is sensitive to
  // stack-into-heap spills).
  //
  // On a touch board inputMgr.update() also walks serviceTouch() → the GT911 I2C
  // read path, which is considerably deeper than analogRead: it peaked at 2088 bytes
  // on the T5S3 (2026-08-17), so 2 KB would overflow there. 4 KB is also the SDK's own
  // figure for a task that calls update() (InputManager::beginAsync creates "fi_input"
  // with a 4096 stack). See docs/contributing/touch-architecture.md, "I2C bus and the
  // input sampler".
  // Watch the btnSampler high-water [MEM] line if changed.
  constexpr uint32_t SAMPLER_STACK_BYTES = FREEINK_CAP_TOUCH ? 4096 : 2048;
  const BaseType_t created =
      xTaskCreate(&HalGPIO::samplerTask, "btnsample", SAMPLER_STACK_BYTES, this, 2, &samplerTaskHandle_);
  if (created != pdPASS || samplerTaskHandle_ == nullptr) {
    LOG_ERR("BTN", "Failed to create input sampler task");
    samplerRunning_.store(false, std::memory_order_release);
    vSemaphoreDelete(samplerStoppedSemaphore_);
    samplerStoppedSemaphore_ = nullptr;
  }
}

void HalGPIO::stopInputSampler() {
  if (!samplerRunning_.load(std::memory_order_acquire)) {
    return;
  }
  // Signal the task to exit on its next wake and self-delete. Avoids vTaskDelete()
  // tearing it down mid-analogRead (which would leak the ADC driver mutex).
  samplerRunning_.store(false, std::memory_order_release);
  xSemaphoreTake(samplerStoppedSemaphore_, portMAX_DELAY);
  samplerTaskHandle_ = nullptr;
  vSemaphoreDelete(samplerStoppedSemaphore_);
  samplerStoppedSemaphore_ = nullptr;
}

bool HalGPIO::hasPendingInput() const {
  bool pending = false;
  portENTER_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  pending = accumPressed_ != 0;
  portEXIT_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  return pending;
}

bool HalGPIO::hasPendingPress(const uint8_t buttonIndex) const {
  bool pending = false;
  portENTER_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  pending = (accumPressed_ & (1u << buttonIndex)) != 0;
  portEXIT_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  return pending;
}

bool HalGPIO::popButtonEdge(ButtonEdge& out) {
  bool got = false;
  portENTER_CRITICAL(&inputMux_);
  if (edgeHead_ != edgeTail_) {
    out = edgeBuf_[edgeHead_];
    edgeHead_ = (edgeHead_ + 1) % EDGE_BUF;
    got = true;
  }
  portEXIT_CRITICAL(&inputMux_);
  return got;
}

bool HalGPIO::popTouchGesture(TouchGesture& out) {
  bool got = false;
  portENTER_CRITICAL(&inputMux_);
  if (gestureHead_ != gestureTail_) {
    out = gestureBuf_[gestureHead_];
    gestureHead_ = (gestureHead_ + 1) % GESTURE_BUF;
    got = true;
  }
  portEXIT_CRITICAL(&inputMux_);
  return got;
}

void HalGPIO::flushTouchEvents() { clearTouchEventState(); }

void HalGPIO::flushButtonEdges() {
  portENTER_CRITICAL(&inputMux_);
  edgeHead_ = 0;
  edgeTail_ = 0;
  accumPressed_ = 0;
  accumReleased_ = 0;
  portEXIT_CRITICAL(&inputMux_);
  snapPressed_ = 0;
  snapReleased_ = 0;
}

void HalGPIO::update() {
  if (!samplerRunning_.load(std::memory_order_acquire)) {
    // Pre-sampler (early boot): sample synchronously on the calling task.
    sampleOnce();
  }
  // Drain the sampler's accumulated edges + latest state into the loop-side
  // snapshot. No edge seen since the last drain is ever lost, regardless of how
  // long this loop iteration took.
  portENTER_CRITICAL(&inputMux_);
  snapState_ = liveState_;
  snapPressed_ = accumPressed_;
  snapReleased_ = accumReleased_;
  accumPressed_ = 0;
  accumReleased_ = 0;
  // One discrete touch event per drain, and the snapshot is REPLACED (not
  // merged) so each is visible for exactly one cycle — the same contract
  // snapPressed_ has. Within that cycle it is a non-consuming peek, which is
  // what lets the gesture classifier look at a tap, decide, and only then
  // suppress it before the screen underneath reads the same tap.
  if (touchEventHead_ != touchEventTail_) {
    snapTouchEvent_ = touchEventBuf_[touchEventHead_];
    touchEventHead_ = (touchEventHead_ + 1) % TOUCH_EVENT_BUF;
  } else {
    snapTouchEvent_ = TouchEvent{};
  }
  snapTouchAtMs_ = snapTouchEvent_.atMs;
  snapTouchReleased_ = touchReleasedPending_;
  touchReleasedPending_ = false;
  portEXIT_CRITICAL(&inputMux_);
  updateUsbState(millis());
}

void HalGPIO::updateUsbState(const unsigned long now) {
  // Host-link check (see the member comment). A single volatile read of a flag
  // the IDF tick hook maintains, so it runs on every call on both devices and is
  // never behind the I2C throttle below — a fresh enumeration must cancel light
  // sleep within a poll or two, or the next slice kills the CDC link again.
  usbHostLinkActive = isUsbHostLinkActive();

  // Throttle USB detection that goes over I2C to the fuel gauge (the X3); see
  // USB_POLL_X3_MS. First call (usbLastPollMs == 0) always polls so boot state is
  // correct. The combined verdict below is still recomputed every call so a
  // host-link-detected attach is not held back by the throttle window.
  if (usbLastPollMs == 0 || !HalCapabilities::usbPresenceFromFuelGauge() || now - usbLastPollMs >= USB_POLL_X3_MS) {
    usbLastPollMs = now;
    usbElectricalConnected = isUsbElectricalConnected();
  }
  const bool connected = usbHostLinkActive || usbElectricalConnected;
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

void HalGPIO::injectPress(const uint8_t buttonIndex, const bool longPress, const uint32_t atMs) {
  if (buttonIndex > BTN_POWER) return;
  // A tap shares one timestamp between press and release, so ButtonEventManager reads it as a
  // Short press. A long tap backdates the press edge past the FSM's threshold instead, so the
  // same code path reads it as a Long one -- the classifier is never told this came from a
  // finger, and the injected hold is indistinguishable from a held key.
  //
  // The backdate is saturated because millis() is small for the first second after boot, and an
  // underflowed press edge would sit ~49 days in the future and never classify at all.
  const uint32_t now = millis();
  const uint32_t releaseAt = atMs != 0 && atMs <= now ? atMs : now;
  const uint32_t holdMs = longPress ? INJECTED_LONG_PRESS_MS : 0;
  const uint32_t pressAt = releaseAt > holdMs ? releaseAt - holdMs : 0;
  portENTER_CRITICAL(&inputMux_);
  accumPressed_ |= (1u << buttonIndex);
  accumReleased_ |= (1u << buttonIndex);
  pushEdgeLocked(buttonIndex, true, pressAt);
  pushEdgeLocked(buttonIndex, false, releaseAt);
  portEXIT_CRITICAL(&inputMux_);
}

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return (snapState_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return (snapPressed_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasAnyPressed() const { return snapPressed_ != 0; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return (snapReleased_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasAnyReleased() const { return snapReleased_ != 0; }

bool HalGPIO::isAnyPressed() const { return snapState_ != 0; }

bool HalGPIO::isDebouncePending() const { return inputMgr.isDebouncePending(); }

unsigned long HalGPIO::getHeldTime() const {
  return samplerRunning_.load(std::memory_order_acquire) ? heldTimeSnapshot_ : inputMgr.getHeldTime();
}

// --- Touch passthrough ------------------------------------------------------
// Deliberately dumb one-liners: no orientation, no logical pixels, no gesture
// naming. See the header for why, and MappedInputManager for the layer that
// interprets these. Every underlying SDK method is already #if FREEINK_CAP_TOUCH
// guarded and inert on non-touch boards.
//
// NOTE (docs/contributing/touch-architecture.md, "I2C bus and the input sampler"): these are all pure
// reads of state the SDK latched during inputMgr.update(). They do NOT touch
// the I2C bus themselves — the GT911 transaction happens inside
// serviceTouch(), which update() calls, i.e. on the btnsample task. The bus
// mutex belongs there, not here.

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::supportsMultiTouch() const { return inputMgr.supportsMultiTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

// The four discrete events answer from the loop-side snapshot, not from the SDK:
// see TouchEvent. The level states below (candidate, held-at) stay live reads,
// because "where is the finger now" has no meaning latched.

bool HalGPIO::wasTouchTap(float& nx, float& ny) const {
  if (snapTouchEvent_.kind != TouchEvent::Kind::Tap) return false;
  nx = snapTouchEvent_.nx;
  ny = snapTouchEvent_.ny;
  return true;
}

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return snapTouchReleased_; }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const {
  if (snapTouchEvent_.kind != TouchEvent::Kind::LongPress) return false;
  nx = snapTouchEvent_.nx;
  ny = snapTouchEvent_.ny;
  return true;
}

void HalGPIO::suppressTouchContact() {
  inputMgr.suppressTouchContact();
  // The SDK latch can only stop events it has not produced yet. Now that events
  // are latched, the tap that follows a long press may ALREADY be queued by the
  // time the long press is acted on — so drop what is held as well, or "ignore
  // the rest of this contact" would not.
  clearTouchEventState();
}

// Everything touch this class holds on the loop side or in its rings. Not
// exposed: the two callers want it for different reasons (suppression, activity
// transitions) and say so themselves.
void HalGPIO::clearTouchEventState() {
  portENTER_CRITICAL(&inputMux_);
  touchEventHead_ = 0;
  touchEventTail_ = 0;
  touchReleasedPending_ = false;
  gestureHead_ = 0;
  gestureTail_ = 0;
  portEXIT_CRITICAL(&inputMux_);
  snapTouchEvent_ = TouchEvent{};
  snapTouchReleased_ = false;
}

unsigned long HalGPIO::lastTouchHeldMs() const {
  // The duration that belongs to the event being reported this cycle, so it
  // cannot describe a different contact than the tap beside it. Falls back to
  // the SDK's latched value when no event is in the snapshot.
  if (snapTouchEvent_.kind == TouchEvent::Kind::Tap || snapTouchEvent_.kind == TouchEvent::Kind::Swipe) {
    return snapTouchEvent_.heldMs;
  }
  return inputMgr.lastTouchHeldMs();
}

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  if (snapTouchEvent_.kind != TouchEvent::Kind::Swipe) return false;
  nxStart = snapTouchEvent_.nx;
  nyStart = snapTouchEvent_.ny;
  nxEnd = snapTouchEvent_.nxEnd;
  nyEnd = snapTouchEvent_.nyEnd;
  return true;
}

bool HalGPIO::wasTouchActivity() const {
  // The latched half matters most here: this feeds the idle timer, and a tap
  // completed while the loop was light-sleeping is exactly the activity that
  // must stop it sleeping. The live read stays OR'd in so a press-DOWN, which is
  // not a latched event, still counts.
  return snapTouchEvent_.kind != TouchEvent::Kind::None || snapTouchReleased_ || inputMgr.wasTouchActivity();
}

unsigned long HalGPIO::waitForStablePowerRelease(unsigned long timeoutMs) {
  // Wait until the raw power-button pin reads HIGH (released) for RELEASE_STABLE_MS
  // consecutive milliseconds.  The InputManager debounce (5 ms) is too short for
  // mechanical switch bounce which can last 10-50 ms, so we bypass it entirely here.
  constexpr unsigned long RELEASE_STABLE_MS = 200;
  const unsigned long waitStart = millis();
  unsigned long stableStart = 0;
  while (true) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart >= RELEASE_STABLE_MS) break;
    } else {
      stableStart = 0;
    }
    // Bounded, and deliberately so. This used to be `while (true)` with no way out, run
    // at the point of no return on the sleep path: the input sampler is already stopped,
    // the panel is already in deep sleep holding the sleep screen, and the loop task is
    // not subscribed to the task WDT (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 is unset
    // and nothing calls esp_task_wdt_add on it), so nothing was going to rescue it. A pin
    // that reads LOW indefinitely — a sticky or dirty switch, a finger that never comes
    // off — therefore froze the device with the sleep screen displayed and every button
    // dead, recoverable only by a reset (issue #155).
    //
    // Sleeping with the button still down is the lesser evil: the worst case is one
    // immediate re-wake (the deep-sleep GPIO trigger is level-LOW), which the boot-side
    // wake gate then classifies like any other press. A permanent freeze has no recovery
    // at all. The caller is told how long we waited so the breadcrumb can record it.
    if (millis() - waitStart >= timeoutMs) {
      LOG_ERR("GPIO", "Power button still held after %lums — sleeping anyway (raw pin LOW)", timeoutMs);
      return millis() - waitStart;
    }
    delay(10);
  }
  const unsigned long waited = millis() - waitStart;
  LOG_DBG("GPIO", "Power button stable-released after %lu ms", waited);
  return waited;
}

bool HalGPIO::isHeldNow(uint8_t buttonIndex, uint8_t confirmSamples) {
  constexpr unsigned long SAMPLE_GAP_MS = 10;
  // Raw values from the deciding sample, logged below. A negative verdict on the ADC
  // ladder has two very different causes — the button genuinely is not held, or its
  // divider reads outside the band the firmware expects — and only the raw value tells
  // them apart. Worth a line: the boot-time callers run before any UI exists to report a
  // misdetected combo, so the log is the sole evidence.
  int raw1 = -1;
  int raw2 = -1;
  int classified1 = -1;
  int classified2 = -1;
  bool held = true;

  for (uint8_t i = 0; i < confirmSamples; i++) {
    if (i > 0) delay(SAMPLE_GAP_MS);
    if (buttonIndex == BTN_POWER) {
      if (digitalRead(InputManager::POWER_BUTTON_PIN) != LOW) {
        held = false;
        break;
      }
      continue;
    }
    InputManager::ButtonAdcSample group1{};
    InputManager::ButtonAdcSample group2{};
    inputMgr.readButtonAdc(group1, group2);
    raw1 = group1.raw;
    raw2 = group2.raw;
    classified1 = group1.button;
    classified2 = group2.button;
    if (group1.button != buttonIndex && group2.button != buttonIndex) {
      held = false;
      break;
    }
  }

  if (buttonIndex != BTN_POWER) {
    LOG_DBG("GPIO", "isHeldNow(btn=%u) -> %d (adc1 raw=%d btn=%d, adc2 raw=%d btn=%d)", buttonIndex, held ? 1 : 0, raw1,
            classified1, raw2, classified2);
  }
  return held;
}

const char* HalGPIO::wakeVerdictName(WakeVerdict verdict) {
  switch (verdict) {
    case WakeVerdict::NotPressed:
      return "not-pressed";
    case WakeVerdict::ShortPress:
      return "short";
    case WakeVerdict::LongHold:
      return "long-hold";
    case WakeVerdict::DoubleClick:
      return "double-click";
    case WakeVerdict::ReleasedEarly:
      return "released-early";
    case WakeVerdict::NoSecondPress:
      return "no-second-press";
  }
  return "?";
}

HalGPIO::WakeCheck HalGPIO::verifyPowerButtonWakeup(WakeGestures gestures, uint16_t requiredDurationMs) {
  // How long the pin must stay HIGH before we conclude the press really ENDED. Long,
  // because concluding it wrongly refuses the wake: a contact bounce mid-hold would return
  // ReleasedEarly and drop the device back into sleep with the user's finger still on the
  // button.
  constexpr unsigned long BOUNCE_TOLERANCE_MS = 100;
  // How long the pin must stay HIGH before a following press counts as the SECOND CLICK of
  // a double rather than the same press bouncing. Much shorter than the tolerance above,
  // and deliberately so: the two thresholds guard opposite errors. Concluding "second
  // click" wrongly only accepts a wake the user was asking for anyway, so this side can
  // afford to be generous, while the reject side cannot.
  //
  // It has to be short. At 100 ms — when this decision shared BOUNCE_TOLERANCE_MS — a
  // second press landing inside the tolerance window never reached the release branch at
  // all: it just refreshed lastSeenPressed, so the gate saw ONE continuous press and then
  // rejected it as ReleasedEarly. A brisk double-click could not wake the device, while
  // the awake FSM (5 ms InputManager debounce) accepted the very same gesture as the
  // double-click that put it to sleep. 30 ms clears typical contact bounce (10-50 ms is
  // the range waitForStablePowerRelease() is written against) and sits well under the
  // ~60 ms floor of a deliberate double-click.
  constexpr unsigned long RELEASE_DEBOUNCE_MS = 30;
  constexpr unsigned long POLL_INTERVAL_MS = 10;
  // Mirrors ButtonEventManager::DOUBLE_WINDOW_MS so a double-click-to-sleep gesture
  // wakes on the same cadence it was configured with. Measured from the first HIGH sample,
  // like the awake FSM measures from the release edge — not from the sample that confirms
  // the release, which would shift the whole window BOUNCE_TOLERANCE_MS late.
  constexpr unsigned long DOUBLE_WINDOW_MS = 300;

  // Must be called before any long-running init (it is the first statement of setup()) so a short
  // press cannot be hidden by boot work: a released button is detected below and returns
  // immediately, which is also why running this on every boot costs nothing on non-button resets.
  const unsigned long gateStart = millis();
  const auto stamp = [](unsigned long ms) { return static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms); };

  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(InputManager::POWER_BUTTON_PIN) != LOW) {
    return {WakeVerdict::NotPressed, stamp(millis()), 0};
  }

  if (gestures.shortAllowed) {
    return {WakeVerdict::ShortPress, stamp(millis()), stamp(millis() - gateStart)};
  }

  // requiredDurationMs (longHold) is compared against millis() directly — time since app
  // start, NOT time since this call. That is deliberate: the press that caused the wake
  // began before setup() ran, so boot time counts toward the hold rather than being
  // charged to the user twice. The caveat is that millis() starts at app init and so
  // excludes the ~200-300 ms bootloader, making the real-world hold needed that much
  // longer than the configured value. Erring long is the safe direction — the failure
  // mode being guarded against is a stray tap waking the device.
  //
  // Watch the first press through to release, classifying it exactly like the awake FSM
  // (ButtonEventManager): held past requiredDurationMs is a long press; released earlier
  // followed by a second press within DOUBLE_WINDOW_MS is a double click; otherwise it's
  // a short tap. Whichever it turns out to be, accept it only if that gesture is enabled.
  unsigned long lastSeenPressed = millis();
  unsigned long releaseSeenAt = 0;  // first HIGH sample of the current release; 0 while held
  while (true) {
    const unsigned long now = millis();
    const uint16_t heldMs = stamp(lastSeenPressed - gateStart);
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      if (releaseSeenAt != 0) {
        // The pin went HIGH and is LOW again: either the second click of a double, or the
        // first press bouncing. The gap decides.
        if (gestures.doubleClick && now - releaseSeenAt >= RELEASE_DEBOUNCE_MS) {
          return {WakeVerdict::DoubleClick, stamp(now), heldMs};
        }
        releaseSeenAt = 0;  // bounce — still the same press
      }
      lastSeenPressed = now;
      if (gestures.longHold && now >= requiredDurationMs) {
        return {WakeVerdict::LongHold, stamp(now), stamp(now - gateStart)};
      }
    } else {
      if (releaseSeenAt == 0) {
        releaseSeenAt = now;
      }
      const unsigned long releasedFor = now - releaseSeenAt;
      // Released before the long-hold threshold. A double-click gesture gets one more
      // chance: a second press within the window after this release, handled above.
      if (!gestures.doubleClick) {
        if (releasedFor >= BOUNCE_TOLERANCE_MS) {
          return {WakeVerdict::ReleasedEarly, stamp(now), heldMs};
        }
      } else if (releasedFor >= DOUBLE_WINDOW_MS) {
        return {WakeVerdict::NoSecondPress, stamp(now), heldMs};
      }
    }
    delay(POLL_INTERVAL_MS);
  }
}

bool HalGPIO::isUsbHostLinkActive() const { return usb_serial_jtag_is_connected(); }

bool HalGPIO::isUsbConnected() const {
  // An enumerated host counts regardless of what the electrical check says. Read
  // the IDF monitor directly rather than the cached member: callers can reach
  // this before the first update() (main.cpp opens the serial log right after
  // gpio.begin()), and the cached value would still be its false initializer.
  return isUsbHostLinkActive() || isUsbElectricalConnected();
}

bool HalGPIO::isUsbElectricalConnected() const {
  if (HalCapabilities::usbPresenceFromFuelGauge()) {
    // X3: GPIO20 is repurposed as I2C SDA, so the X4 pin-level USB detect is
    // unusable here — the I2C pull-ups would always report HIGH. Probe the
    // BQ27220 fuel gauge instead. Charge inference misses a data-only cable and
    // any cable once the battery is full; the host-link check in
    // updateUsbState() covers those.
    //
    // Current() is the ONLY signal trusted here: it is signed, and current
    // flowing INTO the battery (positive, above a noise floor) only happens on a
    // charger.
    //
    // Neither resting flag can be used. DSG=0 means "charging OR merely at
    // rest", so treating it as charger-present latched the charging bolt after
    // unplugging and never recovered (issue #86). FC was then kept as a
    // secondary signal on the theory that it only latches while topped off on a
    // charger — it does not. A detached, fully-charged, idle device reads FC=1
    // with DSG=0 (idle draw sits under the gauge's discharge-detection
    // threshold) and so reported USB permanently connected. That was merely a
    // wrong battery icon until HalPowerManager::lightSleep() started gating on
    // this verdict, at which point it disabled idle light sleep outright —
    // device-observed on an X3 at 100% with no cable attached.
    //
    // The case FC was meant to catch — a cable to a computer with the battery
    // already full — is now covered properly by the host-link check in
    // updateUsbState(), which sees the enumerated link itself rather than
    // inferring it from charge state. The one remaining gap is a dumb wall charger with a
    // full battery: no charge current and no SOF, so no charging indication.
    // That is the honest reading ("charged", not "charging"), and light sleep is
    // safe there because there is no CDC link to lose.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > USB_CHARGE_CURRENT_MIN_MA;
      }
      delay(2);
    }
    return false;
  }
  // Boards without a gauge read a plain GPIO instead: it goes HIGH when USB is
  // connected (X4: U0RXD/GPIO20). Pin from the profile. Unassigned means the
  // board has no electrical detect, so report not-connected and let the SOF path
  // in updateUsbState() be the sole source of truth (X4 Pro today).
  const int8_t usbDetect = BoardConfig::ACTIVE.usbDetect;
  if (usbDetect == BoardConfig::PIN_UNASSIGNED) return false;
  return digitalRead(usbDetect) == HIGH;
}

bool HalGPIO::canDetectUsbElectrically() const {
  // X3 has no usable detect pin — GPIO20 is its gauge SDA — and infers the cable
  // from fuel-gauge charge current instead, so it CAN answer the question.
  if (HalCapabilities::usbPresenceFromFuelGauge()) return true;
  return BoardConfig::ACTIVE.usbDetect != BoardConfig::PIN_UNASSIGNED;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // X3: a POWERON reset can come from either a power-button press (battery-latch
  // MOSFET closes) OR from USB VBUS supplying power to the MCU through a path
  // that bypasses the latch — notably after a Quick Resume sleep, where GPIO13
  // is driven LOW so the MCU is otherwise unpowered. Disambiguate by reading
  // the pulled-up power-button pin directly: GPIO3 reads LOW only while the
  // button is held. If it's not held, treat a USB-connected POWERON as
  // AfterUSBPower so the early handler in setup() puts the device straight
  // back to sleep, matching X4. inputMgr.begin() in gpio.begin() has already
  // configured INPUT_PULLUP on POWER_BUTTON_PIN by the time we're called.
  //
  // Asked as "an Xteink C3 board whose power button gates no battery latch" rather than by
  // name: the latch is what lets the general rule below read POWERON as a press, and
  // POWER_BUTTON_PIN is a C3 pin assumption (see isXteinkDevice()).
  if (isXteinkDevice() && !HalCapabilities::hasButtonGatedBatteryLatch() && wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED &&
      resetReason == ESP_RST_POWERON) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      return WakeupReason::PowerButton;
    }
    return isUsbConnected() ? WakeupReason::AfterUSBPower : WakeupReason::PowerButton;
  }

  const bool usbConnected = isUsbConnected();
  LOG_DBG("GPIO", "getWakeupReason: wakeupCause=%d, resetReason=%d, usbConnected=%d", static_cast<int>(wakeupCause),
          static_cast<int>(resetReason), usbConnected);

  // A GPIO deep-sleep wake means POWER_BUTTON_PIN was pulled LOW, which is a button press by
  // definition — whatever the power source. This clause used to also require usbConnected, on the
  // premise that on battery the MCU is fully powered down so every wake arrives as POWERON.
  // HalPowerManager::startDeepSleep(keepClockAlive=true) broke that premise: with the clock
  // enabled on X4 (enterDeepSleep's keepLpAlive) GPIO13 is held HIGH, the MCU stays powered
  // through sleep, and a battery wake arrives as GPIO+DEEPSLEEP with no USB. That combination
  // matched nothing and fell through to Other, so setup() skipped the hold verification entirely
  // and any tap woke the device. Plugging USB does not pull this pin low, so the AfterUSBPower
  // case below (POWERON + USB) is unaffected.
  // The POWERON arm below infers "the user pressed power" from "we booted with
  // no USB". That inference needs BOTH of the things it assumes:
  //
  //  - a power path the button actually gates, so that being powered at all
  //    implies a press (the C3's GPIO13 battery latch), and
  //  - a trustworthy USB-detect signal, since the whole test is !usbConnected.
  //
  // The T5S3 has neither. Its profile leaves usbDetect PIN_UNASSIGNED, so
  // isUsbConnected() reports false even on USB power, and its power path is the
  // PMIC/PWR button rather than the BOOT button the profile maps as `power`. The
  // result was that pressing the hardware RST button -- a POWERON-class reset
  // with usbConnected forced false -- was classified as a power-button wake,
  // failed the wake gate (the button is not held during a reset), and sent the
  // device straight back to deep sleep in setup(). It looked like RST bricking
  // the board; it was RST being mistaken for a spurious power press.
  //
  // So require a usable USB-detect signal before trusting the inference. A GPIO
  // deep-sleep wake needs no such caveat: the pin was physically pulled low, so
  // it is a press by definition on any board.
  const bool canTrustUsbDetect = BoardConfig::ACTIVE.usbDetect != BoardConfig::PIN_UNASSIGNED;
  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected &&
       canTrustUsbDetect) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP)) {
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
