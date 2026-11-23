#pragma once
#if defined(ARDUINO)
#include_next <Arduino.h>
#else
#include <cstddef>
#include <cstdint>
struct TestESP {
  size_t freeHeap = 400000;
  size_t largestBlock = 200000;
  size_t getFreeHeap() const { return freeHeap; }
  size_t getMaxAllocHeap() const { return largestBlock; }
};
inline TestESP ESP;
inline uint32_t millis() { return 0; }
inline void vTaskDelay(unsigned) {}
#endif
