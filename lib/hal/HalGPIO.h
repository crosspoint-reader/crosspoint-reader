#pragma once

#include <Arduino.h>
#include <InputManager.h>

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
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

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
  // nothing. At >=1 s intervals the energy cost is unmeasurable (~µC/s), so 1 s
  // is chosen for prompt plug/unplug UX (battery icon, light-sleep USB guard /
  // CDC recovery). X4 detection is a single digitalRead and stays per-loop.
  static constexpr unsigned long USB_POLL_X3_MS = 1000;

  // USB-Serial-JTAG SOF activity, sampled by update(): the host sends a SOF
  // frame every 1 ms while the bus is enumerated, so a frame index that moved
  // between two samples means a live host link. Catches what the charge-based
  // X3 check misses: a data-only cable, and any cable once the battery is full
  // (charge current ~0). Samples must be >SOF_SAMPLE_MS apart — update() can be
  // called back-to-back (inner input loops), and adjacent reads would compare
  // equal and flicker the verdict.
  uint16_t lastSofFrameIndex = 0;
  unsigned long sofLastSampleMs = 0;
  bool usbSofActive = false;
  static constexpr unsigned long SOF_SAMPLE_MS = 10;

  // Per-device electrical/charge-inference USB check (fresh read; X3 = BQ27220
  // charge current over I2C, X4 = VBUS-driven level on GPIO20).
  bool isUsbElectricalConnected() const;

  // Shared body of update()/pollUsbState(): SOF sampling + throttled electrical
  // check + combined-verdict edge tracking.
  void updateUsbState(unsigned long now);

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement and the flipped large-step direction in selection activities.
  // Keyed off the active BoardConfig profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  // True while a raw button-state change is still inside the debounce window.
  // The idle loop polls fast while this is set so the confirming sample lands
  // ~10 ms after the first; at the 50 ms light-sleep cadence a short tap can
  // otherwise appear in a single sample and never commit (dropped press).
  bool isDebouncePending() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
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
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify power button was held long enough after wakeup.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Sample USB state without a full input update. Called during setup() BEFORE
  // the first e-ink refresh: the boot paint happens before loop() ever runs
  // update(), so without this the light-sleep slice guards see the unsampled
  // default ("no USB") and sleeping through the boot refresh kills a live CDC
  // link (charge-based X3 detection also reads false whenever the battery is
  // full). Two calls >=SOF_SAMPLE_MS apart establish the SOF verdict; the
  // method itself waits out the floor if called too soon after the last sample.
  void pollUsbState();

  // USB state as sampled by the last update() call.
  // Prefer this in per-loop polling: isUsbConnected() performs a fresh I2C read on X3.
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
