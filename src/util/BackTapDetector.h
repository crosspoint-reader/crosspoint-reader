#pragma once

#include <cstdint>

// Recognises the first tap of one button from raw samples taken every few ms.
// A state counts once it has held for STABLE_MS. A button already down when
// sampling starts is ignored until it has been released.
class BackTapDetector {
 public:
  static constexpr uint32_t STABLE_MS = 15;

  // `valid` false drops the sample and restarts the stability window. Returns
  // true once: on the stable release that ends the first stable press.
  bool update(const uint32_t nowMs, const bool valid, const bool down) {
    if (!valid) {
      candidateKnown = false;
      return false;
    }
    if (!candidateKnown || down != candidateDown) {
      candidateKnown = true;
      candidateDown = down;
      candidateSince = nowMs;
      return false;
    }
    if (nowMs - candidateSince < STABLE_MS) return false;
    if (!armed) {
      armed = !down;
      return false;
    }
    if (down) {
      pressed = true;
      return false;
    }
    if (!pressed || fired) return false;
    fired = true;
    return true;
  }

 private:
  uint32_t candidateSince = 0;
  bool candidateKnown = false;
  bool candidateDown = false;
  bool armed = false;
  bool pressed = false;
  bool fired = false;
};
