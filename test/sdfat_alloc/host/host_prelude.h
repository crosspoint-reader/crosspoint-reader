// Force-included into every SdFat source of the host build (see CMakeLists.txt).
#pragma once
#include <cstdint>
#include <cstring>

// PrintBasic.h declares overloads taking one; the Arduino core defines it.
class __FlashStringHelper;

// FatFileLFN.cpp mixes millis() into the hash of a generated short name.
inline uint32_t millis() {
  static uint32_t ticks = 0;
  return ticks += 7;
}
