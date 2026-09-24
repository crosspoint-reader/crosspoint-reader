#pragma once

#include <atomic>
#include <cstdint>

namespace ReaderUtils {

// Main-loop-owned, one-slot input queue. Only render completion crosses tasks.
class PendingPageTurn {
  int8_t direction = 0;
  std::atomic<uint32_t> requestedRender{0};
  std::atomic<uint32_t> completedRender{0};

 public:
  void enqueue(int8_t value) { direction = value; }
  void clear() { direction = 0; }
  bool hasPending() const { return direction != 0; }
  bool awaitingRender() const {
    return requestedRender.load(std::memory_order_acquire) != completedRender.load(std::memory_order_acquire);
  }
  int8_t take(bool ready) {
    if (!ready || awaitingRender()) return 0;
    const auto result = direction;
    clear();
    return result;
  }
  void requestRender() { requestedRender.fetch_add(1, std::memory_order_release); }
  uint32_t beginRender() const { return requestedRender.load(std::memory_order_acquire); }
  void endRender(uint32_t ticket) { completedRender.store(ticket, std::memory_order_release); }
};

}  // namespace ReaderUtils
