#pragma once

// Minimal host-test stand-in for Arduino.h. Provides just enough of the
// global Arduino surface (delay/millis) that production headers/sources
// reference in passing (e.g. ChapterHtmlSlimParser's popup timing, the image
// decode retry loop) without needing the real Arduino core.

#include <cstdint>

#include "Print.h"
#include "WString.h"

inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }

// Fake ESP32 Arduino core "ESP" global. Reports a comfortably large free
// heap so heap-guard branches (e.g. CssParser's low-memory fallback) never
// trigger during host tests.
struct EspClassStub {
  uint32_t getFreeHeap() const { return 200000; }
};
inline EspClassStub ESP;
