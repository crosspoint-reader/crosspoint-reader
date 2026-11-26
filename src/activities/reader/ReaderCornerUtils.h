#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

#include "CrossPointSettings.h"

namespace ReaderCornerUtils {

// Top-corner shortcuts while reading. Each target is a square whose side is 15%
// of the shorter screen dimension, anchored to the logical top edge, so it
// stays the same size in every orientation. The reader runs a configured
// corner action instead of the page-turn/menu zone under it; a Disabled corner
// keeps the normal tap zones.
constexpr int CORNER_TARGET_PERCENT = 15;

enum class TopCorner : uint8_t { None, Left, Right };

inline int cornerTargetSize(const GfxRenderer& renderer) {
  const int shorter = std::min(renderer.getScreenWidth(), renderer.getScreenHeight());
  return shorter * CORNER_TARGET_PERCENT / 100;
}

inline TopCorner topCornerAt(const GfxRenderer& renderer, const int x, const int y) {
  if (x < 0 || y < 0) return TopCorner::None;
  const int size = cornerTargetSize(renderer);
  if (y >= size) return TopCorner::None;
  if (x < size) return TopCorner::Left;
  if (x >= renderer.getScreenWidth() - size) return TopCorner::Right;
  return TopCorner::None;
}

// Resolve the action bound to a top-corner contact. `hold` selects the
// long-press binding; a point outside the corners maps to Disabled.
inline uint8_t cornerActionAt(const GfxRenderer& renderer, const int x, const int y, const bool hold) {
  switch (topCornerAt(renderer, x, y)) {
    case TopCorner::Left:
      return hold ? SETTINGS.cornerTopLeftHold : SETTINGS.cornerTopLeftTap;
    case TopCorner::Right:
      return hold ? SETTINGS.cornerTopRightHold : SETTINGS.cornerTopRightTap;
    default:
      return CrossPointSettings::CORNER_ACTION_DISABLED;
  }
}

}  // namespace ReaderCornerUtils
