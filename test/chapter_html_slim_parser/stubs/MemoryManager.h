#pragma once

// Host-test stub for the SDK MemoryManager. The parser/layout path calls
// ensureFree() to evict rebuildable caches before large allocations; on the
// host there are no caches and no pressure, so it is a no-op that reports
// success. Matches only the surface the layout code uses.

#include <cstddef>
#include <limits>

namespace freeink {

enum class MemPool : unsigned char { Internal, Psram, Default };

class MemoryManager {
 public:
  static inline size_t largestBlock = std::numeric_limits<size_t>::max();
  static MemoryManager& instance() {
    static MemoryManager inst;
    return inst;
  }
  bool ensureFree(size_t, MemPool = MemPool::Default) { return true; }
  size_t largestFreeBlock(MemPool = MemPool::Default) const { return largestBlock; }

 private:
  MemoryManager() = default;
};

}  // namespace freeink
