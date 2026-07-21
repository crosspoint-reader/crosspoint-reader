#pragma once

#include <cstdint>

class EspStub {
 public:
  uint32_t getFreeHeap() const { return 1024U * 1024U; }
};

inline EspStub ESP;
inline uint32_t millis() { return 0; }
