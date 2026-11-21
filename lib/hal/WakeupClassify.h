#pragma once

#include <cstdint>

// Wake classification for the boot that follows a CrossPoint deep sleep.
// Pure decision policy, host-testable: the device wrapper
// (HalGPIO::getWakeupReason) supplies the raw esp_sleep_get_wakeup_cause() /
// esp_reset_reason() values, the USB-connected probe, and the board's
// cold-boot power-topology rule.
namespace wakeup {

enum class Reason : uint8_t {
  PowerButton,    // Deep-sleep GPIO/ext1 wake (the power button), or a button-
                  // implied cold boot on button-latched boards
  Timer,          // Deep-sleep wake armed through the sleep timer
  AfterFlash,     // Post-flash warm boot over USB
  AfterUSBPower,  // Cold boot on USB power
  Other           // Undefined causes, panics, watchdogs, brownouts
};

// Numeric wake-cause / reset-reason constants pin the stable values of
// esp_sleep_source_t and esp_reset_reason_t (identical across supported
// targets; see the framework's esp_sleep.h / esp_system.h). The device
// wrapper passes its values as ints and guards the pinning with
// static_asserts, so this policy stays host-compilable.
inline constexpr int WAKEUP_UNDEFINED = 0;
inline constexpr int WAKEUP_EXT1 = 3;
inline constexpr int WAKEUP_TIMER = 4;
inline constexpr int WAKEUP_GPIO = 7;

inline constexpr int RST_UNKNOWN = 0;
inline constexpr int RST_POWERON = 1;
inline constexpr int RST_SW = 3;
inline constexpr int RST_DEEPSLEEP = 8;

inline Reason classify(const int wakeupCause, const int resetReason, const bool usbConnected,
                       const bool coldBootImpliesPowerButton) {
  // Deep-sleep exits carry their wake source in both the reset reason and the
  // wakeup cause; both must agree, and a USB plug never overrides them.
  if (resetReason == RST_DEEPSLEEP) {
    if (wakeupCause == WAKEUP_GPIO || wakeupCause == WAKEUP_EXT1) {
      return Reason::PowerButton;
    }
    if (wakeupCause == WAKEUP_TIMER) {
      return Reason::Timer;
    }
  }
  // Cold/warm boots: the cause is undefined and only the reset reason and USB
  // state discriminate.
  if (wakeupCause == WAKEUP_UNDEFINED) {
    if (resetReason == RST_POWERON && !usbConnected && coldBootImpliesPowerButton) {
      return Reason::PowerButton;
    }
    if (resetReason == RST_UNKNOWN && usbConnected) {
      return Reason::AfterFlash;
    }
    if (resetReason == RST_POWERON && usbConnected) {
      return Reason::AfterUSBPower;
    }
  }
  return Reason::Other;
}

}  // namespace wakeup
