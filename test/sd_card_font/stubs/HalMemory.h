#pragma once
#include <HalStorage.h>

#include <cstddef>
class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };
  static inline bool hasPsram = false;
  static HeapStats getPsramHeap() { return {0, hasPsram ? 1024U * 1024U : 0U, 0, 0}; }
  static HeapStats getInternalHeap() { return {ESP.getFreeHeap(), 200 * 1024, 0, 0}; }
};
