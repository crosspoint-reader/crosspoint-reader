#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "components/themes/BaseTheme.h"  // Rect

// Frame-scoped registry of tappable UI elements. Theme draw methods record each
// element's logical rect + id during render() (render task); the input layer
// hit-tests a tap in the next loop() (main task). Lock-free via double buffering:
// render writes the back buffer, publish() flips it live. No per-frame heap.
// Runtime-gated off on boards without touch (setEnabled).
class TouchRegistry {
 public:
  enum Kind : uint8_t { Item = 0, Back = 1, Tab = 2, Cover = 3 };

  static TouchRegistry& getInstance();

  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool isEnabled() const { return enabled_; }

  // Render task: clear the back buffer, append targets, then publish.
  void beginFrame();
  void add(const Rect& rect, int id, Kind kind);
  void publish();

  // Main/loop task: find the topmost target of `kind` containing the logical point.
  // Returns true and writes its id to outId on a hit.
  bool hitTest(int x, int y, Kind kind, int& outId) const;

 private:
  static constexpr size_t CAPACITY = 64;  // worst case is the keyboard grid (~45 keys)

  struct Target {
    Rect rect;
    int16_t id;
    uint8_t kind;
  };

  uint8_t backIndex() const { return live_.load(std::memory_order_relaxed) ^ 1u; }

  std::array<std::array<Target, CAPACITY>, 2> buffers_{};
  std::array<size_t, 2> counts_{0, 0};
  std::atomic<uint8_t> live_{0};
  bool enabled_ = false;
};
