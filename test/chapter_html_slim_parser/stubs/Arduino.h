#pragma once

#include <cstdint>

inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }

struct EspClassStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
};

inline EspClassStub ESP;
