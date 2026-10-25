#pragma once
#include <cstddef>

namespace fake {
inline size_t syncHeap = 68000;
inline bool syncBufferAllocated = false;
}  // namespace fake
struct HalMemory {
  struct Heap {
    size_t freeBytes;
    size_t largestBlockBytes;
  };
  static Heap getDefaultHeap() {
    const size_t available = fake::syncHeap - (fake::syncBufferAllocated ? 32768 : 0);
    return {available, available};
  }
};
