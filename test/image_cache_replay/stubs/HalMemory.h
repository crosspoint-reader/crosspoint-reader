#pragma once
#include <cstddef>

namespace HalMemory {
struct Heap {
  size_t freeBytes;
};
inline Heap getInternalHeap() { return {1024 * 1024}; }
}  // namespace HalMemory
