#pragma once

#include <cstddef>
#include <cstdint>

namespace pluginevents {

// Cooperative limits for one drain, shared by all plugins and by auth/retry.
// Zero disables each limit independently. The clock is monotonic modulo
// uint32_t (millis()); elapsed subtraction also works when it wraps.
// A blocking DNS/TCP connect can exceed maxElapsedMs.
class DrainBudget {
 public:
  using Clock = uint32_t (*)();
  DrainBudget(size_t attemptLimit, uint32_t elapsedLimit, Clock clockFn)
      : maxAttempts(attemptLimit), maxElapsedMs(elapsedLimit), clock(clockFn), started(elapsedLimit ? clockFn() : 0) {}

  bool hasDeadline() const { return maxElapsedMs != 0; }
  bool timeAvailable() const { return !hasDeadline() || static_cast<uint32_t>(clock() - started) < maxElapsedMs; }
  bool available() const { return (maxAttempts == 0 || attempts < maxAttempts) && timeAvailable(); }
  bool beginAttempt() {
    if (!available()) return false;
    ++attempts;
    return true;
  }

 private:
  size_t maxAttempts;
  uint32_t maxElapsedMs;
  Clock clock;
  uint32_t started;
  size_t attempts = 0;
};

}  // namespace pluginevents
