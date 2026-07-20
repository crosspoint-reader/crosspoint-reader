#pragma once

#include <cstdint>

struct ReadingSessionSample {
  uint32_t seconds = 0;
  bool forwardPageRead = false;
};

// Measures only the interval during which a successfully rendered page is
// visible. Unsigned subtraction intentionally keeps millis() wrap-safe.
class ReadingSessionTracker {
 public:
  static constexpr uint32_t MIN_FORWARD_PAGE_DWELL_MS = 2000;
  static constexpr uint32_t MAX_ACTIVE_INTERVAL_MS = 5 * 60 * 1000;

  bool pageVisible(const uint32_t nowMs) {
    if (active) return false;
    active = true;
    visibleSinceMs = nowMs;
    return true;
  }

  ReadingSessionSample stop(const uint32_t nowMs, const bool forwardPageTurn) {
    if (!active) return {};
    active = false;

    const uint32_t elapsedMs = nowMs - visibleSinceMs;
    if (elapsedMs > MAX_ACTIVE_INTERVAL_MS) return {};

    ReadingSessionSample sample;
    sample.seconds = elapsedMs / 1000;
    sample.forwardPageRead = forwardPageTurn && elapsedMs >= MIN_FORWARD_PAGE_DWELL_MS;
    return sample;
  }

  bool discardIfIdle(const uint32_t nowMs) {
    if (!active || nowMs - visibleSinceMs <= MAX_ACTIVE_INTERVAL_MS) return false;
    active = false;
    return true;
  }

  bool isActive() const { return active; }

 private:
  bool active = false;
  uint32_t visibleSinceMs = 0;
};
