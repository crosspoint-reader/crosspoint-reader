#include "TouchRegistry.h"

TouchRegistry& TouchRegistry::getInstance() {
  static TouchRegistry instance;
  return instance;
}

void TouchRegistry::beginFrame() {
  if (!enabled_) return;
  counts_[backIndex()] = 0;
}

void TouchRegistry::add(const Rect& rect, int id, Kind kind) {
  if (!enabled_) return;
  const uint8_t b = backIndex();
  size_t& n = counts_[b];
  if (n >= CAPACITY) return;  // silently drop overflow; CAPACITY sized for a screen
  buffers_[b][n] = Target{rect, static_cast<int16_t>(id), static_cast<uint8_t>(kind)};
  ++n;
}

void TouchRegistry::publish() {
  if (!enabled_) return;
  // Flip live to the buffer we just filled. Release so the reader sees the writes.
  live_.store(backIndex(), std::memory_order_release);
}

bool TouchRegistry::hitTest(int x, int y, Kind kind, int& outId) const {
  if (!enabled_) return false;
  const uint8_t b = live_.load(std::memory_order_acquire);
  const size_t n = counts_[b];
  const auto& buf = buffers_[b];
  // Last-registered wins (topmost / most recently drawn).
  for (size_t i = n; i-- > 0;) {
    if (buf[i].kind == kind && buf[i].rect.contains(x, y)) {
      outId = buf[i].id;
      return true;
    }
  }
  return false;
}
