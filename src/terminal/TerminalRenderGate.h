#pragma once

#include <atomic>
#include <cstdint>

// Coalesces render requests while preserving one redraw that arrives during an
// in-flight E Ink update. A single atomic state avoids the race where a
// producer latches a redraw just as the renderer marks itself idle.
class TerminalRenderGate {
 public:
  void reset() { state.store(0, std::memory_order_release); }

  // Returns true when the caller claimed an idle renderer and must notify the
  // ActivityManager. Requests made while rendering are coalesced into one
  // replay and return false.
  bool request() {
    uint8_t current = state.load(std::memory_order_acquire);
    for (;;) {
      const bool scheduled = (current & SCHEDULED) != 0;
      const uint8_t desired = scheduled ? static_cast<uint8_t>(current | RERUN) : SCHEDULED;
      if (state.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return !scheduled;
      }
    }
  }

  // Completes the active render. Returns true when a request arrived during
  // that render; the caller must immediately notify ActivityManager again.
  bool complete() {
    uint8_t current = state.load(std::memory_order_acquire);
    for (;;) {
      const bool replay = (current & RERUN) != 0;
      const uint8_t desired = replay ? SCHEDULED : 0;
      if (state.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return replay;
      }
    }
  }

 private:
  static constexpr uint8_t SCHEDULED = 1 << 0;
  static constexpr uint8_t RERUN = 1 << 1;

  std::atomic<uint8_t> state{0};
};
