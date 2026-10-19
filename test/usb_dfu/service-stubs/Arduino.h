#pragma once
#include <chrono>
#include <thread>
inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return 1 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}
inline void delay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
