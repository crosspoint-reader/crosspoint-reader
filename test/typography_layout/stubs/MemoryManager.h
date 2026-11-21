#pragma once

#include <cstddef>
#include <functional>

namespace freeink {

enum class MemPool : unsigned char { Internal, Psram, Default };

class MemoryManager {
 public:
  struct Sink {
    const char* name = nullptr;
    int priority = 0;
    std::function<size_t(size_t)> evict;
  };

  static MemoryManager& instance() {
    static MemoryManager inst;
    return inst;
  }
  void registerSink(const Sink&) {}
  size_t freeBytes() const { return 200 * 1024; }
  bool ensureFree(size_t, MemPool = MemPool::Default) { return true; }

 private:
  MemoryManager() = default;
};

}  // namespace freeink
