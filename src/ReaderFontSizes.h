#pragma once

#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "fontIds.h"

// Reader font size is stored as an actual point size (see CrossPointSettings::
// fontPointSize), not an abstract Small/Medium/Large slot. The selectable sizes
// therefore come from whichever family is active: the built-in or vector set
// below, or the .cpfont files a user installed for an SD family.

// The built-in Noto Sans family is compiled in at exactly these
// point sizes (see the global font objects in main.cpp).
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12, 14, 16, 18};

// Selects a built-in size without loading it; returns 0 for other font families.
inline int builtinReaderFontVariant(const int fontId, const float scale) {
  static constexpr int FAMILIES[][std::size(BUILTIN_READER_POINT_SIZES)] = {
      {NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_18_FONT_ID},
  };
  for (const int* family : FAMILIES) {
    const int* end = family + std::size(BUILTIN_READER_POINT_SIZES);
    const int* base = std::find(family, end, fontId);
    if (base == end) continue;
    const float target = BUILTIN_READER_POINT_SIZES[base - family] * scale;
    size_t pick = scale < 1.0f ? 0 : static_cast<size_t>(base - family);
    for (size_t i = 0; i < std::size(BUILTIN_READER_POINT_SIZES); ++i) {
      const float points = BUILTIN_READER_POINT_SIZES[i];
      // Shrinking rounds down; growing takes the nearest available size.
      if (scale < 1.0f ? points <= target
                       : std::fabs(points - target) < std::fabs(BUILTIN_READER_POINT_SIZES[pick] - target)) {
        pick = i;
      }
    }
    return family[pick];
  }
  return 0;
}

// Vector (.ttf/.otf) fonts offer every whole point size from 8 through 22.
inline constexpr uint8_t VECTOR_READER_POINT_SIZES[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};

// Point sizes selectable for the active reader font, ascending: the vector set
// or installed .cpfont sizes for a known SD family, otherwise the built-in set.
// Never returns empty.
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}
