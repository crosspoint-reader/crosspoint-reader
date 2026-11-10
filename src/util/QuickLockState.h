#pragma once

#include <atomic>
#include <cstdint>

class QuickLockState {
 public:
  void lock(const uint32_t nowMs) {
    lastActivityMs = nowMs;
    locked.store(true, std::memory_order_relaxed);
  }

  void unlock() { locked.store(false, std::memory_order_relaxed); }

  bool isLocked() const { return locked.load(std::memory_order_relaxed); }

  // The sleep timeout counts from the lock or from the last remote page turn.
  void noteActivity(const uint32_t nowMs) { lastActivityMs = nowMs; }

  bool shouldSleep(const uint32_t nowMs, const uint32_t timeoutMs) const {
    return isLocked() && timeoutMs > 0U && static_cast<uint32_t>(nowMs - lastActivityMs) >= timeoutMs;
  }

 private:
  // Also read by the render task, for the status bar icon and the badge.
  std::atomic<bool> locked{false};
  uint32_t lastActivityMs = 0U;
};
