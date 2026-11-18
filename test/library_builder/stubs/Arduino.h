#pragma once

#include <cstdint>

#include "HalStorage.h"

inline uint32_t millis() {
  static uint32_t clock = 0;
  return ++clock;
}

inline void delay(unsigned) { fake::delays++; }

struct FakeEsp {
  uint32_t getFreeHeap() const { return 100000; }
  uint32_t getMaxAllocHeap() const { return 80000; }
};

inline FakeEsp ESP;
