#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
