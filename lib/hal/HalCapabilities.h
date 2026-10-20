#pragma once

#include <BoardConfig.h>

// Ask what the board CAN DO, not what it IS.
//
// Why this file exists (see docs/contributing/board-support.md,
// "Ask what the board can do"):
//
// `HalGPIO::deviceIsX3()` has ~38 call sites and is a stand-in for at least six
// unrelated questions — is there a hardware RTC, is there an IMU, is the battery
// read over I2C or ADC, does the panel need a half-refresh settle, how is USB
// presence detected, and where are the physical buttons. That works only while
// exactly two boards exist and every question happens to correlate.
//
// It breaks silently on a third board. On X4 Pro `deviceIsX3()` returns false,
// so every one of those questions takes the **X4** branch by default. The board
// does not fail loudly; it is subtly wrong in six places that each look like an
// unrelated bug. Widening `DeviceType` to {X3, X4, X4Pro, LilyGo} multiplies the
// conflation by four rather than removing it.
//
// So each predicate below answers ONE question, from the active board profile.
// Adding a board then means filling in a profile, not editing call sites.
//
// The call sites have moved over: board questions to the predicates here, panel
// questions to the SDK's display (GfxRenderer::fastRefreshNeedsHostFrame(),
// asyncRefreshHoldsFrameBuffer(), ghostsUnderFastRefresh()), key placement to
// ButtonGeometry. Each answers as `deviceIsX3()` did on every shipped board.
//
// `deviceIsX3()` itself stays for the one question it IS: which C3 board this is.
// The dual X3+X4 binary's runtime detection in HalGPIO::begin() sets it, selects
// the SDK profile from it, and HalDisplay::begin() hands it to the panel driver.
// Nothing else should ask.
namespace HalCapabilities {

// --- Sensors -----------------------------------------------------------------

// A battery-backed real-time clock is present. X3 has a DS3231, X4 Pro a BM8563;
// X4 has none and falls back to the ESP32 RTC across deep sleep.
inline bool hasHardwareRtc() { return BoardConfig::ACTIVE.sensors.rtcType != BoardConfig::RtcType::None; }

// Which RTC, for the register/address differences. DS3231 is 0x68, the
// PCF8563-compatible BM8563 is 0x51 — same bus, different protocol, so a caller
// that only checks "is there an RTC" would talk nonsense to the wrong one.
inline BoardConfig::RtcType rtcType() { return BoardConfig::ACTIVE.sensors.rtcType; }

// An IMU is present, so tilt gestures are physically possible. X3 carries a
// QMI8658; X4 and X4 Pro carry nothing.
inline bool hasTiltSensor() { return BoardConfig::ACTIVE.sensors.imuType != BoardConfig::ImuType::None; }

// --- Battery -----------------------------------------------------------------

// Charge comes from an I2C fuel gauge rather than an ADC divider. X3 has a
// BQ27220, X4 Pro a CW2017; X4 reads an ADC pin. These are mutually exclusive
// and each needs a completely different read path.
inline bool hasI2cFuelGauge() { return BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0; }

// Charge comes from an ADC pin. Note this is not simply !hasI2cFuelGauge(): a
// profile may legitimately have neither, in which case there is no battery
// reading at all and the UI should not pretend otherwise.
inline bool hasAdcBattery() { return !hasI2cFuelGauge() && BoardConfig::ACTIVE.batteryAdc >= 0; }

// Any battery reading is available, by either route.
inline bool hasBatteryReading() { return hasI2cFuelGauge() || hasAdcBattery(); }

// --- Power and USB -----------------------------------------------------------

// The power button closes a battery MOSFET that a latch pin then holds (the X4's
// GPIO13; PowerConfig::latch0GatesBattery). So being powered at all means the
// button was pressed, and driving the latch LOW is the battery power-off. The
// X4 Pro also carries a latch0, but it keeps a peripheral rail up, so it is not
// this; the X3 has no latch at all, and its GPIO13 is the SD rail enable.
inline bool hasButtonGatedBatteryLatch() {
  return BoardConfig::ACTIVE.power.latch0GatesBattery &&
         BoardConfig::ACTIVE.power.latch0 != BoardConfig::PIN_UNASSIGNED;
}

// USB presence has to come from the fuel gauge's charge current: the board has a
// BQ27220 to read, no charger IC that could report the input rail, and no pin
// that reads the cable. That is the X3. Its GPIO20 is the gauge's I2C data line,
// which reads HIGH from the bus pull-ups whatever the cable does; the profile
// used to list it as usbDetect anyway and now leaves usbDetect unassigned
// (Free-Ink edcc629), so both forms count as "no pin".
//
// The charger test is what keeps the T5S3 out. It has a BQ27220 and no usbDetect
// either, but its BQ25896 answers for the rail, and a cable inferred from charge
// current would close the serial-log gate on a full battery (main.cpp).
//
// Reading the gauge is an I2C transaction, so it is polled on a throttle rather
// than every loop like a pin.
constexpr bool usbPresenceFromFuelGauge(const BoardConfig::BoardProfile& p) {
  const bool noDetectPin = p.usbDetect == BoardConfig::PIN_UNASSIGNED || p.usbDetect == p.batteryGauge.i2cSda;
  return p.batteryGauge.gaugeAddr != 0 && p.batteryGauge.gaugeType == BoardConfig::GaugeType::Bq27220 &&
         p.batteryGauge.chargerAddr == 0 && noDetectPin;
}
inline bool usbPresenceFromFuelGauge() { return usbPresenceFromFuelGauge(BoardConfig::ACTIVE); }

// Pinned against the shipped profiles, so an SDK resync that changes any of the
// fields above fails the build here instead of silently turning the X3's
// charging indication and USB wake handling off.
static_assert(usbPresenceFromFuelGauge(BoardConfig::XTEINK_X3) &&
                  usbPresenceFromFuelGauge(BoardConfig::XTEINK_X3_UC8279),
              "X3: USB presence must come from the BQ27220");
static_assert(!usbPresenceFromFuelGauge(BoardConfig::XTEINK_X4) &&
                  !usbPresenceFromFuelGauge(BoardConfig::XTEINK_X4_PRO) &&
                  !usbPresenceFromFuelGauge(BoardConfig::LILYGO_T5S3),
              "only the X3 infers USB from the fuel gauge");

// --- Display -----------------------------------------------------------------

// A genuine silicon quirk, and the one case B0 allows to key on the controller:
// the UC8253 needs extra settle passes after a half refresh or it ghosts. This
// is a property of the panel controller, not of "being an X3" — a future UC8253
// board would need it too, and an SSD1677 X3 variant would not.
inline bool panelNeedsHalfRefreshSettle() {
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8253;
}

// The controller develops grayscale from a swappable LUT, so the fast/OEM
// waveform trade-off is a real choice. SSD1677 uses a factory waveform and has
// nothing to swap. Mirrors SettingRequires::SelectableGrayscaleLut.
inline bool hasSelectableGrayscaleLut() {
  return BoardConfig::ACTIVE.displayController != BoardConfig::DisplayController::SSD1677;
}

// The panel visibly fades in direct sunlight unless it is powered down between
// refreshes, so the "sunlight fading fix" is worth offering here.
//
// Recorded per board rather than derived: it is a property of the glass and the
// enclosure, not of the controller, and it is not something the firmware can
// probe. Mirrors SettingRequires::SunlightFadingPanel.
inline bool panelFadesInSunlight() { return BoardConfig::ACTIVE.panelFadesInSunlight; }

// --- Input / chrome ----------------------------------------------------------

// A touch panel is present. Thin alias over the SDK predicate so call sites read
// consistently with the rest of this header.
inline bool hasTouch() { return BoardConfig::hasTouch(); }

// The board's touch controller reports more than one contact (GT911), as its profile says.
// HalGPIO::supportsMultiTouch() answers the same for the controller that answered its probe
// at this boot.
inline bool hasMultiTouch() { return BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911; }

// The touch controller reports a capacitive Home key (X4 Pro). On that board it
// is the only "back" the hardware has — there is no back button.
inline bool hasHomeKey() { return BoardConfig::hasHomeKey(); }

// Dedicated Back and Confirm buttons exist in hardware.
//
// This is the one that bites hardest on X4 Pro: its InputPins are
// {back=-1, confirm=-1, left=-1, right=-1, up=0, down=7, power=3}, i.e. two nav
// keys and power, with back and confirm coming from the GT911 (touch plus the
// capacitive Home key). Any UI that assumes a Back button — button hint strips,
// "press Back to cancel" prompts, the escape path out of a modal — has no
// hardware to bind to there. Note every supported board has SOME button
// topology, so InputStyle has no None value; presence is the wrong question and
// this is the right one.
inline bool hasBackAndConfirmButtons() {
  const BoardConfig::InputPins& in = BoardConfig::ACTIVE.input;
  return in.back != BoardConfig::PIN_UNASSIGNED && in.confirm != BoardConfig::PIN_UNASSIGNED;
}

// The physical button topology, for the places that genuinely need to know the
// arrangement rather than a yes/no capability.
inline BoardConfig::InputStyle inputStyle() { return BoardConfig::ACTIVE.inputStyle; }

// True when the SD card is reached over SPI, false when the board drives the
// slot through the native SDMMC peripheral (X4 Pro, X4 Classic).
//
// Everything in the SdPins struct -- spiHz, cs, the shared-bus locking -- is
// dead configuration on an SDMMC board, which mounts through SdmmcBlockDevice
// and never touches those pins. Code that tunes or reports the SPI path must
// ask this first, or it ends up announcing an SPI clock on a board that has no
// SD SPI bus.
inline bool sdUsesSpi() { return BoardConfig::ACTIVE.sdmmc.busWidth == 0; }

// The MCU pin the ROM samples at reset to choose boot vs. download mode. Holding
// it LOW while the chip comes out of reset enters ROM download mode, so firmware
// never runs and no boot-time key combo on that pin can ever be observed.
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
inline constexpr int8_t BOOT_MODE_STRAP_PIN = 9;
#else  // ESP32 / ESP32-S2 / ESP32-S3
inline constexpr int8_t BOOT_MODE_STRAP_PIN = 0;
#endif

// True when this board wires its Up key to that strapping pin, so a boot-time
// combo must use a different key. The X4 Pro and X4 Classic both put Up on
// GPIO0; on those, "hold Up + power" drops the device into ROM download mode
// instead of into the firmware check the user was aiming for, which looks
// exactly like a dead device. Derived from the profile rather than a board list,
// so the next board that reuses the strap is covered without an edit.
//
// The InputStyle test is load-bearing, not defensive: on the X3/X4 ladder the
// InputPins fields hold LADDER INDICES, not GPIO numbers (X4's tuple is
// {0,1,2,3,4,5,...}), so comparing input.up against a pin number there is a
// category error that happens to be false today only because no index reaches 9.
inline bool upKeyIsBootStrap() {
  return BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder &&
         BoardConfig::ACTIVE.input.up == BOOT_MODE_STRAP_PIN;
}

// Chrome scale factor for finger-sized targets. 1.0 on button boards, 1.2 on the
// touch boards. Read by ThemeMetrics in touch phase 5.
inline float uiScale() { return BoardConfig::ACTIVE.uiScale; }

// A frontlight is present (PWM or PM1-driven).
inline bool hasFrontlight() { return BoardConfig::hasPwmFrontlight(); }

// The frontlight has a second (warm) channel, so colour temperature is
// adjustable rather than brightness-only.
inline bool hasColorTemperature() {
  return hasFrontlight() && BoardConfig::ACTIVE.frontlight.gpioWarm != BoardConfig::PIN_UNASSIGNED;
}

// Human-readable board name for the UI. The profile's own `name` is a build
// slug ("xteink_x4", "lilygo_t5s3") and would read badly on screen, so the
// display spelling lives here. Deliberately exhaustive with no default: a new
// board then fails to compile rather than silently reporting the wrong device,
// which is exactly the failure this replaces -- the old deviceIsX3() ternary
// called every S3 board an "X4".
inline const char* boardDisplayName(BoardConfig::Board board) {
  switch (board) {
    case BoardConfig::Board::XteinkX4:
      return "X4";
    // Same board and glass as the X3, differing only in the panel controller,
    // which is reported on its own row.
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return "X3";
    case BoardConfig::Board::XteinkX4Pro:
      return "X4 Pro";
    case BoardConfig::Board::M5StackPaperColor:
      return "M5Paper Color";
    case BoardConfig::Board::MurphyM3:
      return "Murphy M3";
    case BoardConfig::Board::MurphyM4:
      return "Murphy M4";
    case BoardConfig::Board::DeLink:
      return "de-link";
    case BoardConfig::Board::LilyGoT5S3:
      return "T5 S3 Pro";
    case BoardConfig::Board::M5PaperV11:
      return "M5Paper v1.1";
    case BoardConfig::Board::Sticky:
      return "Sticky";
    case BoardConfig::Board::PaperMono:
      return "Paper Mono";
    case BoardConfig::Board::M5PaperS3:
      return "M5Paper S3";
  }
  return "Unknown";
}

}  // namespace HalCapabilities

// Boards whose support layer owns bus bring-up.
//
// The LilyGo T5S3 ships a board-support library (freeink-sdk BoardT5S3) whose
// begin() owns BOTH bus inits: beginI2C() starts Wire on the board's pins, and
// prepareSdBus() drives LORA_CS and SD_CS HIGH before SPI.begin(). That
// deselect matters — the board puts a LoRa radio on the SD's SPI bus, and an
// undriven chip select there corrupts SD transactions. It also configures the
// BOOT button and registers the PCA9535 expander's user button through
// InputManager::setButtonHook().
//
// Where such a layer exists, the HAL must NOT also start those buses: a second
// Wire.begin() on one port breaks the ESP-IDF i2c_master driver outright, and a
// second SPI.begin() would undo the CS deselects.
// This is a board NAME rather than a capability, deliberately and narrowly:
// board-support glue is per-board by definition, unlike the hardware questions
// HalCapabilities answers. Keeping it to this one macro is what stops it
// spreading into the conflation B0 removed.
#define BOARD_SUPPORT_OWNS_BUSES (FREEINK_DEVICE_LILYGO)
