#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdFontData.h"

class GlyphStreamCodec {
 public:
  static constexpr uint8_t MAX_GLYPH_WIDTH = 63;
  static constexpr uint8_t MAX_GLYPH_HEIGHT = 46;
  static constexpr size_t SCRATCH_PLANE_SIZE = 3 * 1024;

  static bool decode(const EpdFontData* fontData, uint32_t glyphIndex, bool wantGray, uint8_t* scratchA,
                     uint8_t* scratchB, uint8_t* output, size_t outputCapacity);

  static size_t packedSize(uint8_t width, uint8_t height, bool is2Bit);
};
