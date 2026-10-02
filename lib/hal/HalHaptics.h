#pragma once

#include <BoardConfig.h>

#include <cstdint>

#if FREEINK_CAP_HAPTIC && !CROSSPOINT_EMULATED
#include <HapticManager.h>
#endif

// Application-owned feedback policy; the SDK owns motor timing and PWM.
class HalHaptics {
  // Persisted option order: Low, Medium, High. This scales motor PWM duty,
  // not pulse duration; perceived strength depends on the motor and battery.
  // Metalio's motor did not respond to short pulses at 50% in device testing.
  // Use approximately 60%, 75%, and 88%; validate the new floor on hardware.
  static constexpr uint8_t gain(uint8_t level) { return level == 0 ? 153 : level == 1 ? 191 : 224; }

 public:
  static void longPress(bool enabled, uint8_t intensity) {
#if FREEINK_CAP_HAPTIC && !CROSSPOINT_EMULATED
    if (!enabled) return;
    auto& motor = HapticManager::getInstance();
    motor.setEnabled(true);
    motor.setIntensity(gain(intensity));
    // A double pulse distinguishes recognition from the initial touch pulse.
    // The SDK copies this fixed pattern and plays it without blocking input.
    static constexpr HapticManager::Step pattern[] = {{45, 255}, {55, 0}, {45, 255}};
    if (motor.begin()) motor.play(pattern, 3);
#else
    (void)enabled;
    (void)intensity;
#endif
  }

  static void feedback(bool enabled, bool pressed, uint8_t intensity) {
#if FREEINK_CAP_HAPTIC && !CROSSPOINT_EMULATED
    auto& motor = HapticManager::getInstance();
    motor.setEnabled(enabled);
    // Initialize lazily: Off does not allocate a timer or attach PWM.
    if (enabled && pressed && motor.begin()) {
      motor.setIntensity(gain(intensity));
      motor.pulse();
    }
#else
    (void)enabled;
    (void)pressed;
    (void)intensity;
#endif
  }
};
