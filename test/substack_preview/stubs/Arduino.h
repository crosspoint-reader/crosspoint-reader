#pragma once

// Host-build stand-in for Arduino.h. Covers exactly what FontDecompressor.cpp
// and GfxRenderer.cpp need on the real hardware build (which pulls in a real
// Arduino.h transitively) but a bare host compiler doesn't provide for free:
// millis() timing, and ESP.restart() in GfxRenderer.cpp's unreachable
// out-of-memory recovery branch (FrameBufferLoan::end()) — never actually
// called by substack_preview.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

inline uint32_t millis() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline uint32_t micros() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now() - start).count());
}

struct EspClass {
  [[noreturn]] void restart() { std::abort(); }
};
inline EspClass ESP;
