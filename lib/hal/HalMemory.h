#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };

  struct PsramDeleter {
    void operator()(uint8_t* buffer) const;
  };
  using PsramBuffer = std::unique_ptr<uint8_t[], PsramDeleter>;
  // Never falls back to internal RAM. Null on allocation failure or absent PSRAM.
  static PsramBuffer allocatePsram(size_t bytes);

  // Default-capability memory includes PSRAM when registered with the allocator.
  static HeapStats getDefaultHeap();
  static HeapStats getInternalHeap();
  static HeapStats getPsramHeap();
};
