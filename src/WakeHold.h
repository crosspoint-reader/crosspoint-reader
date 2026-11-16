#pragma once

#include <atomic>

// "Always Long Press to Wake": a wake from the power button has to be a deliberate hold, so a bump
// in a bag or pocket cannot wake the device. Wakes from anything else, and the restarts the
// firmware does on its own, are not button presses and are never held up.
constexpr unsigned long kWakeHoldMs = 500;

constexpr bool wakeNeedsLongPress(const bool settingOn, const bool powerButtonWake, const bool silentReboot) {
  return settingOn && powerButtonWake && !silentReboot;
}

// The verdict once the setting is known. If the background watch never ran, an early release
// could have gone unseen, so the button must still be down now. That may turn away a hold that
// was let go just after the deadline, but it can never refuse a button that is being held.
constexpr bool wakeHoldAccepted(const bool wakeVerified, const bool heldWhileWatched, const bool watchRan,
                                const bool pressedNow) {
  return wakeVerified && heldWhileWatched && (watchRan || pressedNow);
}

// Follows the button from the moment the wake is known to be a press. The setting that asks for
// the hold is only readable later, once the SD card is up, so the button is sampled in the
// background meanwhile: letting go before kWakeHoldMs breaks the hold for good, and letting go
// after it does not, however late the answer is read.
class WakeHoldWatch {
 public:
  void sample(const bool pressed, const unsigned long nowMs) {
    if (!pressed && nowMs < kWakeHoldMs) released.store(true, std::memory_order_relaxed);
  }
  bool held() const { return !released.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> released{false};
};
