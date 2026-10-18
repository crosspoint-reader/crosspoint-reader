#pragma once

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "components/themes/BaseTheme.h"

namespace selectionGeometry {
// Consume press edges only: display refresh can outlast the input's held-state snapshot.
inline int horizontalIndex(const int selected, const int count, const bool leftPressed, const bool rightPressed,
                           const bool readingOrderRtl = false) {
  if (count <= 0) return -1;
  const int direction = static_cast<int>(rightPressed) - static_cast<int>(leftPressed);
  return std::clamp(selected + (readingOrderRtl ? -direction : direction), 0, count - 1);
}

inline int keepVisible(const int position, const int extent, const int start, const int size) {
  return position < start ? start - position : std::min(0, start + size - position - extent);
}

inline bool atBottomEdge(const Rect safe, const int lineHeight, const int x, const int y) {
  return x >= safe.x && x < safe.x + safe.width && y >= safe.y + safe.height - lineHeight;
}

inline bool nearerWord(const Rect candidate, const Rect current, const int x, const int y) {
  const auto distance = [x, y](const Rect word) {
    return std::pair{std::abs(word.y + word.height / 2 - y), std::max({word.x - x, 0, x - (word.x + word.width)})};
  };
  return distance(candidate) < distance(current);
}

inline Rect actions(const Rect safe, const int textTop, const int height, const int gap, const int preferredWidth = 0,
                    const int anchorX = 0) {
  const int width = preferredWidth > 0 ? std::min(safe.width, preferredWidth) : safe.width;
  return Rect{std::clamp(anchorX, safe.x, safe.x + safe.width - width), std::max(safe.y, textTop - height - gap), width,
              height};
}

inline Rect button(const Rect actions, const int index, const int padding) {
  const int width = (actions.width - padding * 4) / 3;
  return Rect{actions.x + padding + index * (width + padding), actions.y + padding, width,
              actions.height - padding * 2};
}

inline bool contains(const Rect rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

inline int actionAt(const Rect actions, const int padding, const int x, const int y) {
  for (int i = 0; i < 3; ++i) {
    if (contains(button(actions, i, padding), x, y)) return i;
  }
  return -1;
}
}  // namespace selectionGeometry
