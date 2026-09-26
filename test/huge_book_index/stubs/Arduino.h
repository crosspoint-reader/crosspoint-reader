#pragma once

#include <HeapCap.h>

#include <cstdint>

// Free heap as the device would report it: whatever the test cap leaves.
struct EspHostStub {
  uint32_t getFreeHeap() const {
    const size_t v = heapcap::available();
    return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
  }
};

inline EspHostStub ESP;
