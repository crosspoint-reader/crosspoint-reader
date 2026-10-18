#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
// StateOfCharge() is deliberately absent: percentage now comes from the SDK's
// BatteryMonitor, which dispatches on the profile's GaugeType instead of
// assuming every gauge is a BQ27220. Current() stays because the X3's USB
// charge inference reads it directly, and the X3 really does carry a BQ27220.
#define I2C_ADDR_BQ27220 0x55    // Fuel gauge I2C address
#define BQ27220_CUR_REG 0x0C     // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08    // Voltage() command code (mV)
#define BQ27220_FLAGS_REG 0x0A   // BatteryStatus() / Flags() command code (bit0=DSG, bit9=FC)
#define BQ27220_FLAG_DSG 0x0001  // DSG bit: 1 = discharging, 0 = charging OR merely at rest
#define BQ27220_FLAG_FC 0x0200   // FC bit: 1 = fully charged (only latches while on the charger)
// Minimum Current() reading (mA, positive = into the battery) that counts as
// "on the charger". A small guard band above 0 keeps gauge noise around rest
// from being read as charging.
#define USB_CHARGE_CURRENT_MIN_MA 5

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

namespace X3GPIO {
// Read a 16-bit little-endian I2C register. Returns false on bus error.
bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue);
}  // namespace X3GPIO

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;
  unsigned long usbLastPollMs = 0;
  bool usbElectricalConnected = false;  // last result of the per-device electrical/charge check

  // X3 USB detection is a BQ27220 I2C read (~0.3-1 ms of awake CPU per call);
  // polled every loop it costs a few percent of the light-sleep idle floor for
  // nothing. At >=1 s intervals the energy cost is unmeasurable, so 1 s is
  // chosen for prompt plug/unplug UX (battery icon, the light-sleep USB guard).
  // X4 detection is a single digitalRead and stays per-loop.
  static constexpr unsigned long USB_POLL_X3_MS = 1000;

  // Live USB host link, straight from the IDF's SOF monitor
  // (usb_serial_jtag_is_connected(), maintained by a FreeRTOS tick hook that
  // watches the SOF interrupt bit with a 3 ms no-SOF tolerance). Catches what
  // the charge-based X3 check misses: a data-only cable, and any cable once the
  // battery is full (charge current ~0). Both matter for
  // HalPowerManager::lightSleep(), which must not halt the chip out from under
  // an enumerated CDC link — and for main.cpp, which only opens the serial log
  // when a host is present.
  //
  // This used to be sampled here by diffing USB_SERIAL_JTAG.fram_num: that index
  // is 11 bits and wraps every 2.048 s, so any sampling cadence landing on a
  // multiple of that read two equal values and reported "no host" while a
  // monitor was attached. The IDF hook has no such blind spot and costs nothing
  // to read.
  bool usbHostLinkActive = false;

  // Per-device electrical/charge-inference USB check (fresh read; X3 = BQ27220
  // charge current over I2C, X4 = VBUS-driven level on GPIO20).
  bool isUsbElectricalConnected() const;

  // SOF sampling + throttled electrical check + combined-verdict edge tracking.
  void updateUsbState(unsigned long now);

 public:
  enum class DeviceType : uint8_t { X4, X3 };

  // A single debounced button transition captured by the background sampler.
  // `button` is a raw BTN_* index; `pressed` is true for a press edge, false for
  // a release edge; `timeMs` is the millis() value at the time the edge was
  // detected, so consumers can classify Short/Long/Double independent of how
  // often the loop task gets around to draining the queue.
  struct ButtonEdge {
    uint8_t button = 0;
    bool pressed = false;
    uint32_t timeMs = 0;
  };

  // A completed multi-touch gesture, as the SDK classified it. Panel-native
  // normalized centre, no orientation applied — MappedInputManager gives these
  // screen meaning, exactly as it does for taps and swipes.
  //
  // Unlike the single-contact events above these are LATCHED on the sampler
  // task and queued, because they are one-shot flags the SDK clears on its next
  // update() — a ~10 ms life. A pinch is precisely the gesture a reader makes
  // while the loop task is inside an e-paper refresh, so passing them straight
  // through would drop most of them. Buttons have been latched for this reason
  // since the sampler was introduced; these follow the same rule.
  struct TouchGesture {
    enum class Kind : uint8_t { Pinch, Rotate };
    Kind kind = Kind::Pinch;
    // Pinch: end separation over start separation, so < 1 is a pinch in and > 1
    // a spread. Rotate: signed degrees, positive = clockwise on screen.
    float magnitude = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    uint32_t timeMs = 0;
  };

 private:
  DeviceType _deviceType = DeviceType::X4;

  // ---- Background button sampler (see HalGPIO.cpp) ----------------------------
  // The buttons are read by polling the ADC; the loop task can be blocked for
  // hundreds of ms at a time (e.g. a sliced background section build whose slice
  // overshoots its budget on a heavy page), so polling once per loop iteration
  // drops presses that begin and end inside one slow iteration. A dedicated task
  // samples + debounces on a fixed ~10ms cadence regardless of loop progress and
  // latches every edge for the loop task to drain.
  TaskHandle_t samplerTaskHandle_ = nullptr;
  SemaphoreHandle_t samplerStoppedSemaphore_ = nullptr;
  std::atomic<bool> samplerRunning_{false};
  portMUX_TYPE inputMux_ = portMUX_INITIALIZER_UNLOCKED;

  // Shared sampler→loop state, guarded by inputMux_.
  uint8_t accumPressed_ = 0;   // press edges seen since the last update() drain
  uint8_t accumReleased_ = 0;  // release edges seen since the last update() drain
  uint8_t liveState_ = 0;      // latest debounced button state
  unsigned long heldTimeSnapshot_ = 0;
  static constexpr int EDGE_BUF = 32;
  ButtonEdge edgeBuf_[EDGE_BUF] = {};
  int edgeHead_ = 0;
  int edgeTail_ = 0;
  // A discrete single-contact touch event, latched by the sampler so it survives
  // a loop tick that took longer than the SDK's flags live.
  //
  // The SDK reports tap / swipe / long-press as one-shot flags cleared by its
  // next update(), i.e. a ~10 ms life at our sampler cadence. That is shorter
  // than an e-paper refresh and — measured on device — shorter than the gap
  // between polls once the idle governor starts light-sleeping at
  // IDLE_LIGHT_SLEEP_MS, which is exactly when someone is sitting on a menu
  // deciding what to tap. One tap in fifteen was being dropped that way.
  //
  // Buttons have been latched into a ring for this reason since the sampler
  // existed; this is the same model. update() moves ONE event into the loop-side
  // snapshot, where it stays visible for that whole cycle — which matters,
  // because the accessors are non-consuming peeks that several layers read in
  // turn (the gesture classifier decides before the reader sees the same tap).
  struct TouchEvent {
    enum class Kind : uint8_t { None, Tap, Swipe, LongPress };
    Kind kind = Kind::None;
    // Tap and LongPress: the touch-down point. Swipe: where it started.
    float nx = 0.0f;
    float ny = 0.0f;
    float nxEnd = 0.0f;  // Swipe only.
    float nyEnd = 0.0f;
    uint16_t heldMs = 0;  // contact duration, latched at release (0 for LongPress)
    // millis() when the sampler latched it: a tap's release. The loop may get to the event much
    // later (a screen refresh can hold it up), so anything timing taps reads this, not the clock.
    uint32_t atMs = 0;
  };
  // Four is deliberately small. A multi-touch gesture ends when the fingers
  // leave the glass, so they cannot arrive faster than a person can lift and
  // replace two fingers; a backlog deeper than this means the loop has been gone
  // long enough that acting on the oldest entry would surprise the user more
  // than dropping it.
  static constexpr int GESTURE_BUF = 4;
  TouchGesture gestureBuf_[GESTURE_BUF] = {};
  int gestureHead_ = 0;
  int gestureTail_ = 0;
  // Same size and the same argument as GESTURE_BUF: contacts end when a finger
  // leaves the glass, so they cannot queue faster than someone can tap, and a
  // deeper backlog means acting on the oldest would surprise more than dropping
  // it.
  static constexpr int TOUCH_EVENT_BUF = 4;
  TouchEvent touchEventBuf_[TOUCH_EVENT_BUF] = {};
  int touchEventHead_ = 0;
  int touchEventTail_ = 0;
  bool touchReleasedPending_ = false;  // release edges seen since the last drain

  // Loop-side snapshot refreshed by update(); only the loop task reads/writes these.
  uint8_t snapState_ = 0;
  // Counts of injected inputs, and of those update() has handed over; under inputMux_.
  uint32_t injectedInputs_ = 0;
  uint32_t deliveredInputs_ = 0;
  uint8_t snapPressed_ = 0;
  uint8_t snapReleased_ = 0;
  TouchEvent snapTouchEvent_{};
  // atMs of the touch event drained this cycle, kept when a reader consumes the event itself.
  uint32_t snapTouchAtMs_ = 0;
  bool snapTouchReleased_ = false;

  // Capacitive home key -> the nav buttons the board physically lacks: tap emits
  // CONFIRM, hold emits BACK. Each is enabled independently in begin(), so a
  // board owning one of those pins keeps it (see the synthesis in sampleOnce).
  // homeKeyHeld_ marks a gesture in progress, whose role is not yet known.
  bool homeKeyDrivesConfirm_ = false;
  bool homeKeyDrivesBack_ = false;
  bool homeKeyHeld_ = false;

  // Edge detection for the BUTTON_TRACE touch line, so a resting finger logs once
  // rather than every sampler pass. Bring-up scaffolding; goes with the trace.
  bool touchTraceWasHeld_ = false;
  // Where the contact started and where it last was, so the release line can
  // report how far the finger actually travelled. The SDK's thresholds are
  // private and its endpoints are only filled in when it has already decided the
  // contact WAS a swipe — which is no help when the question is why it decided
  // it was not.
  float touchTraceDownNx_ = 0.0f;
  float touchTraceDownNy_ = 0.0f;
  float touchTraceLastNx_ = 0.0f;
  float touchTraceLastNy_ = 0.0f;

  void sampleOnce();
  void pushEdgeLocked(uint8_t button, bool pressed, uint32_t timeMs);
  // Drain the SDK's one-shot multi-touch flags into gestureBuf_. Called from
  // sampleOnce() with inputMux_ NOT held: it reads the SDK, which the critical
  // section must not do.
  void latchTouchGestures(uint32_t timeMs);
  // Drain the SDK's one-shot single-contact flags into touchEventBuf_. Runs
  // beside latchTouchGestures() and under the same rule: reads the SDK outside
  // inputMux_, takes the lock only to push.
  void latchTouchEvents();
  // Queue one event for the next update(); drops it when the ring is full. Takes inputMux_.
  void pushTouchEvent(const TouchEvent& event);
  // Drop every latched and snapshotted touch event, gestures included.
  void clearTouchEventState();
  static void samplerTask(void* arg);

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }

  // True only on the Xteink C3 boards (X3, X3/UC8279, X4). Guards the pin
  // assumptions that hold for the C3 hardware but not for its S3 siblings or
  // third-party boards — chiefly GPIO13, which is a power control here and an
  // ordinary bus signal elsewhere. Note this is NOT !deviceIsX3(): X4 Pro is an
  // Xteink board but not an Xteink *C3* board, and _deviceType only ever
  // distinguishes the two C3 variants.
  bool isXteinkDevice() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  // Synthesize a complete press+release of a raw button from something that is not a
  // button -- currently a tap on the on-screen hint strip, which is the only way to
  // reach Back/Confirm on a board whose nav cluster is PIN_UNASSIGNED.
  //
  // Same mechanism the capacitive home key uses inside update() (accumulators plus a
  // matching edge pair, so both the wasPressed()/wasReleased() bitmask consumers and
  // ButtonEventManager's press-type FSM see it), lifted to a public entry point for
  // callers outside this class. Takes a RAW index, not a logical Button: remapping is
  // applied downstream, so an injected BTN_BACK follows the user's button mapping
  // exactly as the physical key would.
  //
  // `longPress` backdates the press edge past ButtonEventManager's long threshold, so the FSM
  // classifies it Long. Without it a whole class of actions is unreachable on a touch-only
  // board: X4 Pro has no Back or Confirm pin and its capacitive home key emits press and
  // release in the same pass, so NOTHING there can produce a hold -- and the file browser's
  // context menu, the recents view toggle, remove and book info all sit on one.
  //
  // `atMs` dates a tap at the moment it happened (TouchEvent::atMs) rather than when the loop got
  // to it, so two quick taps on a box stay a double-tap. 0 means now.
  void injectPress(uint8_t buttonIndex, bool longPress = false, uint32_t atMs = 0);

  // How far back an injected long press dates its press edge. Must stay above
  // ButtonEventManager::LONG_PRESS_MS, which lives in src/ and cannot be included from the HAL;
  // ActivityManager static_asserts the relationship where both are visible.
  static constexpr uint32_t INJECTED_LONG_PRESS_MS = 1500;
  // Synthesize a finished contact from something that is not a finger -- the serial CMD:TOUCH and
  // CMD:SWIPE: a tap at (nx, ny), or a swipe from there to (nxEnd, nyEnd), in the same normalized
  // panel-native coordinates the controller reports (GfxRenderer::logicalToTap() makes them). It
  // joins the queue the sampler fills, so everything downstream (gestures, list taps, the reader's
  // tap zones) reads it as the SDK's own classification. The live "finger is down" queries never
  // see it. No-op on a board without a touch panel.
  void injectTouch(float nx, float ny);
  void injectSwipe(float nx, float ny, float nxEnd, float nyEnd);
  // True while something injected (a press or a touch above) has not yet been handed to the loop
  // by update(). The serial CMD:IDLE uses it to know a press it sent has reached the screen.
  bool injectedInputPending() const;
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  // True while ANY button is currently held down. Distinct from wasAnyPressed(), which is
  // edge-triggered and therefore false for every loop iteration after the initial press —
  // a held button looks exactly like an idle device to an edge-only check.
  bool isAnyPressed() const;
  // True while a raw button-state change is still inside the debounce window.
  // The idle loop polls fast while this is set so the confirming sample lands
  // ~10 ms after the first; at the light-sleep cadence a short tap can
  // otherwise appear in a single sample and never commit (dropped press).
  bool isDebouncePending() const;
  unsigned long getHeldTime() const;

  // --- Touch passthrough ----------------------------------------------------
  // Raw passthrough to the SDK's touch machine: normalized 0..1 coordinates in
  // the PANEL's native frame, with no orientation applied. Interpretation
  // (orientation mapping, logical pixels, gesture semantics, hit tests) belongs
  // to MappedInputManager, not here — this layer only exposes the SDK
  // capability, per the repo's HAL rule.
  //
  // Names and signatures are copied verbatim from upstream/develop's HalGPIO so
  // the layers above stay diff-comparable; see
  // docs/contributing/touch-architecture.md, "Layers".
  //
  // No FREEINK_CAP_TOUCH guards are needed: every InputManager touch method is
  // already guarded inside the SDK and compiles to an inert false/0 on non-touch
  // boards, so the C3 pays nothing for these.
  bool hasTouch() const;
  // True only on a controller that reports more than one contact (GT911). Gates
  // the two-finger gestures out of the settings screen on single-contact panels,
  // where they could never fire.
  bool supportsMultiTouch() const;
  // Capacitive Home key reported by the touch controller (X4 Pro). The tap
  // event fires on release and excludes a long hold.
  bool hasHomeKey() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  // One-shot long-press, fired by the SDK classifier while the finger is still
  // down (stationary contact held past its threshold). Position = touch-down
  // point. Callers that act on it should suppressTouchContact() so the lift
  // cannot also tap.
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact (its continued hold and its
  // release edge). Self-clears once the contact ends.
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  // When the touch event drained this cycle happened (its TouchEvent::atMs), 0 if none.
  uint32_t lastTouchEventAtMs() const { return snapTouchAtMs_; }
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  // Drain one queued multi-touch gesture (FIFO). Returns false when empty.
  bool popTouchGesture(TouchGesture& out);
  // Drop every queued touch event — gestures AND single-contact taps, swipes and
  // long presses, latched and snapshotted alike. For activity transitions, so a
  // tap made on the screen being left cannot act on the one being entered, which
  // matters more now that these outlive the tick they happened in.
  void flushTouchEvents();
  // Coarse "the user touched the screen" signal — the touch analogue of
  // wasAnyPressed(). Feed this into the idle/sleep timer so touch counts as
  // activity (phase 3).
  bool wasTouchActivity() const;

  // Start/stop the background sampler. startInputSampler() must be called once
  // input handling is wanted (end of setup, after the boot-time power-button
  // handling that drives inputMgr.update() directly). Until then update() falls
  // back to sampling synchronously. stopInputSampler() is called before deep
  // sleep so no ADC read races with the display/power-rail teardown.
  void startInputSampler();
  void stopInputSampler();

  // True if any button press has been sampled since the last update() drain.
  // Safe to call from any context (e.g. mid-stall inside loop()); does not
  // consume the edge — update() will still see it on the next main-loop tick.
  bool hasPendingInput() const;
  // hasPendingInput() for one raw button, under the same contract: any context, non-consuming.
  // For a caller stuck inside a long blocking call that must react to one specific button
  // without draining the rest away from the loop (File Transfer's upload cancel).
  bool hasPendingPress(uint8_t buttonIndex) const;

  // Pop the oldest queued button edge (FIFO). Returns false when the queue is
  // empty. Drained by ButtonEventManager to drive its press-type FSM.
  bool popButtonEdge(ButtonEdge& out);
  // Drop all queued edges and pending accumulated press/release bits. Called on
  // activity transitions so stale input does not bleed across screens.
  void flushButtonEdges();

  // Minimum free stack (bytes) the sampler task has ever had, for right-sizing its
  // stack allocation. 0 when the sampler is not running.
  UBaseType_t samplerStackHighWater() const {
    return samplerTaskHandle_ ? uxTaskGetStackHighWaterMark(samplerTaskHandle_) : 0;
  }

  // Wait until the raw power-button GPIO reads HIGH (released) for a sustained period.
  // Uses the raw pin directly instead of the InputManager debounced state to avoid
  // the 5 ms debounce being fooled by mechanical switch bounce during release.
  // Block until the raw power pin has read HIGH for 200 ms straight, or `timeoutMs`
  // elapses — whichever comes first. Returns how long it waited; compare against
  // `timeoutMs` to tell a clean release from a give-up. Bypasses the InputManager
  // debounce deliberately (5 ms is too short for 10-50 ms mechanical release bounce),
  // and reads the pin directly so it works with the input sampler stopped.
  unsigned long waitForStablePowerRelease(unsigned long timeoutMs = POWER_RELEASE_TIMEOUT_MS);

  // Ceiling for the wait above. Long enough that no deliberate hold reaches it (the
  // longest configurable power-hold-to-sleep gesture is well under this), short enough
  // that a stuck pin costs the user seconds rather than a reset.
  static constexpr unsigned long POWER_RELEASE_TIMEOUT_MS = 5000;

  // "Is this button held right now?", answered from fresh hardware samples rather than
  // the cache isPressed() reads. The six front buttons are resistor dividers on two ADC
  // pins, so a sample is a plain analogRead plus a band lookup — no debounce state to
  // warm up, which is what makes this usable during boot before update() has run enough
  // times to populate the cache. Power is a digital pin and is read directly.
  //
  // Every sample must agree, so a transient cannot produce a false positive; a genuinely
  // held button is stable and passes. Blocks for confirmSamples * 10 ms.
  bool isHeldNow(uint8_t buttonIndex, uint8_t confirmSamples = 4);

  // Which power-button press pattern(s) are accepted as an intentional wake. Mirrors
  // whichever press type(s) the user configured to put the device to sleep — several
  // can be set at once (e.g. both short AND long press power off), in which case any
  // one of the enabled gestures wakes it.
  struct WakeGestures {
    bool shortAllowed = false;  // any press/release, however brief, wakes the device
    bool doubleClick = false;   // two presses with releases within the double-click window
    bool longHold = true;       // sustained hold of requiredDurationMs (the safe default)
  };

  // What the boot-time wake gate actually observed on the power pin. Reported so a
  // "the button did nothing" report can be told apart from a slow boot: the rejecting
  // verdicts each name a different user error (let go too soon, second click missed).
  enum class WakeVerdict : uint8_t {
    NotPressed,     // pin already HIGH when the gate first sampled it
    ShortPress,     // accepted: short-press-to-sleep is configured, any press wakes
    LongHold,       // accepted: held past requiredDurationMs
    DoubleClick,    // accepted: released early, second press inside the window
    ReleasedEarly,  // rejected: released before the threshold, double-click not configured
    NoSecondPress,  // rejected: released early, double-click armed, second press never came
  };

  struct WakeCheck {
    WakeVerdict verdict = WakeVerdict::NotPressed;
    uint16_t decidedAtMs = 0;  // millis() when the verdict was reached
    // How long the gate itself saw the first press, NOT the user's real hold: the press
    // began before the app did, so the bootloader and everything up to the gate is not
    // counted. Use decidedAtMs for "when the device committed to waking".
    uint16_t heldMs = 0;

    bool accepted() const {
      return verdict == WakeVerdict::ShortPress || verdict == WakeVerdict::LongHold ||
             verdict == WakeVerdict::DoubleClick;
    }
  };

  // Human-readable name of a verdict, for logging. Points at a string literal.
  static const char* wakeVerdictName(WakeVerdict verdict);

  // Verify the raw power button was pressed in one of the patterns enabled by `gestures`,
  // mirroring the press type(s) configured to put the device to sleep. requiredDurationMs
  // is only used by the longHold gesture.
  // Call as early as possible so cold-boot initialization cannot hide a short press.
  WakeCheck verifyPowerButtonWakeup(WakeGestures gestures, uint16_t requiredDurationMs);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Enumerated USB host link only (SOF activity), with no charge-state
  // inference mixed in. Separated out so a caller that needs to know *why* the
  // verdict came out the way it did — the serial-log gate's diagnostic — can
  // report the two terms apart.
  bool isUsbHostLinkActive() const;

  // Whether this board can observe a cable at all WITHOUT an enumerated host —
  // an X3-style gauge-current inference or a usbDetect pin. False on the
  // native-USB boards whose profile leaves usbDetect unassigned (X4 Pro, T5S3):
  // there, a false from isUsbConnected() means "no host enumerated right now",
  // NOT "no cable", so nothing may treat it as a negative signal.
  bool canDetectUsbElectrically() const;

  // USB state as sampled by the last update() call. Prefer this in per-loop
  // polling: isUsbConnected() performs a fresh I2C read on X3.
  bool isUsbConnectedCached() const { return lastUsbConnected; }

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
