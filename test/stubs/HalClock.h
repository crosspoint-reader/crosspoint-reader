#pragma once

#include <chrono>

inline unsigned long millis() {
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  return static_cast<unsigned long>(ms.count());
}

class HalClock {
 public:
  void begin() {}
};

inline HalClock halClock;
