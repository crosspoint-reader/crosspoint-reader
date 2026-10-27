#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t freeHeap = UINT32_MAX;
  uint32_t maxAllocHeap = UINT32_MAX;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAllocHeap; }
};

inline EspHostStub ESP;
inline void vTaskDelay(uint32_t) {}
