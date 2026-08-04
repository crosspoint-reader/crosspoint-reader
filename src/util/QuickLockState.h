#pragma once

#include <cstdint>

class QuickLockState {
 public:
  enum class Transition { PauseReading, ResumeReading };

  Transition toggle(const uint32_t nowMs) {
    locked = !locked;
    lockedAtMs = locked ? nowMs : 0U;
    return locked ? Transition::PauseReading : Transition::ResumeReading;
  }

  bool isLocked() const { return locked; }

  bool shouldSleep(const uint32_t nowMs, const uint32_t timeoutMs) const {
    return locked && timeoutMs > 0U && static_cast<uint32_t>(nowMs - lockedAtMs) >= timeoutMs;
  }

 private:
  bool locked = false;
  uint32_t lockedAtMs = 0U;
};
