#pragma once

#include <cstdint>

// Host stand-in for the Arduino ESP object: no heap pressure, except that a
// test can make one getFreeHeap() call report an empty heap, which refuses
// exactly that shaper allocation.
struct EspHostStub {
  int64_t emptyAtCall = -1;
  int64_t freeHeapCalls = 0;
  uint32_t getFreeHeap() { return freeHeapCalls++ == emptyAtCall ? 0 : UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;

// The clock only moves when a test advances it.
inline unsigned long hostMillis = 0;
inline unsigned long millis() { return hostMillis; }
