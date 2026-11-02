#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace edge_swipe {
enum class Edge : uint8_t { None, Left, Right, Top, Bottom };

inline Edge classify(int sx, int sy, int ex, int ey, int width, int height) {
  const int dx = ex - sx, dy = ey - sy;
  if (dx * dx + dy * dy <= 2499) return Edge::None;
  const int band = std::clamp(std::min(width, height) * 8 / 100, 20, 48);
  if (std::abs(dx) > std::abs(dy)) {
    if (dx > 0 && sx <= band) return Edge::Left;
  } else {
    if (dy > 0 && sy <= band) return Edge::Top;
    if (dy < 0 && sy >= height - band) return Edge::Bottom;
  }
  return Edge::None;
}
class Recognizer {
 public:
  void begin(int x, int y, uint32_t now = 0) {
    startX = x;
    startY = y;
    tracking = true;
    startedAt = now;
    previousDistanceSquared = 0;
  }
  void cancel() { tracking = false; }
  bool active() const { return tracking; }
  Edge sample(int x, int y, int width, int height, bool released = false, uint32_t now = 0) {
    if (!tracking) return Edge::None;
    const int dx = x - startX, dy = y - startY;
    if (released) tracking = false;
    const int distanceSquared = dx * dx + dy * dy;
    if (distanceSquared <= 2499) {
      // The reference hands stationary holds to long-press before a later swipe.
      if (!released && now - startedAt >= 500 && previousDistanceSquared <= 225) tracking = false;
      previousDistanceSquared = distanceSquared;
      return Edge::None;
    }
    tracking = false;
    return classify(startX, startY, x, y, width, height);
  }

 private:
  int startX = 0, startY = 0;
  bool tracking = false;
  uint32_t startedAt = 0;
  int previousDistanceSquared = 0;
};
}  // namespace edge_swipe
