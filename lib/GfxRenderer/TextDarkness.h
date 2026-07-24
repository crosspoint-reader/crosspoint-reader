#pragma once

#include <cstdint>

namespace GlyphDarkness {

constexpr uint8_t NORMAL = 0;
constexpr uint8_t DARK = 1;
constexpr uint8_t EXTRA_DARK = 2;

constexpr uint8_t mapLevel(const uint8_t level, const uint8_t darkness) {
  if (level > 3) return level;
  if (darkness == DARK && level == 2) return 1;
  if (darkness == EXTRA_DARK && level > 0 && level < 3) return level - 1;
  return level;
}

}  // namespace GlyphDarkness
