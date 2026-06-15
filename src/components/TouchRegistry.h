#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "components/themes/BaseTheme.h"  // Rect

// Frame-scoped registry of tappable UI elements. Theme draw methods record each
// interactive element's LOGICAL rect + id during render() (on the render task);
// the input layer hit-tests a tap against it during the next loop() (main task).
//
// Lock-free single-writer (render) / single-reader (loop) via double buffering:
// render writes the back buffer and publish() atomically flips it to live, so the
// reader never observes a half-built frame. No per-frame heap allocation.
//
// Runtime-gated: disabled on boards without touch (setEnabled(gpio.hasTouch())),
// so add()/hitTest() are a single branch on the C3.
class TouchRegistry {
 public:
  enum Kind : uint8_t { Item = 0, Back = 1 };

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
  static constexpr size_t CAPACITY = 48;  // worst case excl. keyboard grid (Phase 2)

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
