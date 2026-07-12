#pragma once
// Arduino API stubs for native test compilation

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using String = std::string;

inline unsigned long millis() {
  static unsigned long t = 0;
  return t += 10;
}
inline void delay(unsigned long) {}
inline void yield() {}

// ESP32 random
inline uint32_t esp_random() { return (uint32_t)rand(); }
