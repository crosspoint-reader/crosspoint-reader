#pragma once

// What the TTF engine draws for every glyph of Bookerly Regular at 14 pt, as one FNV-1a hash. The host test
// (test/ttf_rasterizer/TtfVsFreeTypeTest.cpp) computes it from the source font, and so does the device bench
// (bench/ttf_raster_main.cpp): equal hashes mean the device draws exactly what the host checked against
// FreeType. A change to the engine's output changes it; update kBookerlyRegular14pt from the host test.

#include <cstdint>

#include "EpdFontData.h"

namespace ttf_golden {

constexpr uint32_t kSeed = 2166136261u;
constexpr uint32_t kBookerlyRegular14pt = 0xDE1AEEA7u;

inline uint32_t mix(const uint32_t hash, const uint8_t byte) { return (hash ^ byte) * 16777619u; }

/// Folds one glyph into `hash`: its index, its metrics (each as 4 little-endian bytes) and its bitmap.
inline uint32_t hashGlyph(uint32_t hash, const uint16_t glyph, const EpdGlyph& metrics, const uint8_t* bitmap) {
  const int32_t fields[] = {glyph, metrics.width, metrics.height, metrics.advanceX, metrics.left, metrics.top};
  for (const int32_t field : fields) {
    for (int shift = 0; shift < 32; shift += 8) hash = mix(hash, static_cast<uint8_t>(field >> shift));
  }
  for (uint32_t i = 0; i < metrics.dataLength; ++i) hash = mix(hash, bitmap[i]);
  return hash;
}

}  // namespace ttf_golden
