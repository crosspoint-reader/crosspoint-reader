#pragma once

#include <cstdint>

// Host stand-in for the Arduino ESP object: no heap pressure.
struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
