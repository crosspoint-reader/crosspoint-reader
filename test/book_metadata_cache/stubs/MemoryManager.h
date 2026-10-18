#pragma once
#include <cstddef>
#include <limits>
namespace freeink {
class MemoryManager {
 public:
  inline static size_t largestBlock = std::numeric_limits<size_t>::max();
  static MemoryManager& instance() {
    static MemoryManager manager;
    return manager;
  }
  size_t largestFreeBlock() const { return largestBlock; }
};
}  // namespace freeink
