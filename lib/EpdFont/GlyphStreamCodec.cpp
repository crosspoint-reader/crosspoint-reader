#include "GlyphStreamCodec.h"

#include <Logging.h>

#include <cstring>

#include "builtinFonts/glyphStreamModel.h"

static_assert(GLYPH_STREAM_MODEL_VERSION == 1, "GlyphStream model version mismatch");

namespace {

constexpr uint8_t HAS_REF_FLAG = 0x80;
constexpr uint8_t RAW_FLAG = 0x40;
constexpr uint8_t RESERVED_MASK = 0x30;
constexpr int SHIFT_BIAS = 7;
constexpr uint32_t TOP_VALUE = 1U << 24;

struct StreamInfo {
  const uint8_t* payload = nullptr;
  uint16_t payloadSize = 0;
  uint16_t baseIndex = 0;
  int8_t shift = 0;
  bool hasReference = false;
  bool raw = false;
};

class RangeDecoder {
 public:
  RangeDecoder(const uint8_t* payload, const uint16_t payloadSize) : payload(payload), payloadSize(payloadSize) {
    for (uint8_t index = 0; index < 4; ++index) {
      code = (code << 8) | readByte();
    }
  }

  uint8_t decodeBit(const uint16_t probability) {
    const uint32_t bound = (range >> 16) * probability;
    uint8_t bit = 0;
    if (code < bound) {
      bit = 1;
      range = bound;
    } else {
      code -= bound;
      range -= bound;
    }
    while (range < TOP_VALUE) {
      code = (code << 8) | readByte();
      range <<= 8;
    }
    return bit;
  }

 private:
  uint8_t readByte() {
    if (position >= payloadSize) return 0;
    return payload[position++];
  }

  const uint8_t* payload;
  uint16_t payloadSize;
  uint16_t position = 0;
  uint32_t range = 0xFFFFFFFF;
  uint32_t code = 0;
};

uint32_t glyphCount(const EpdFontData* fontData) {
  if (fontData->intervalCount == 0 || fontData->intervals == nullptr) return 0;
  const EpdUnicodeInterval& last = fontData->intervals[fontData->intervalCount - 1];
  return last.offset + last.last - last.first + 1;
}

bool dimensionsFit(const EpdGlyph& glyph) {
  return glyph.width <= GlyphStreamCodec::MAX_GLYPH_WIDTH && glyph.height <= GlyphStreamCodec::MAX_GLYPH_HEIGHT &&
         static_cast<size_t>(glyph.width) * glyph.height <= GlyphStreamCodec::SCRATCH_PLANE_SIZE;
}

bool parseStream(const EpdFontData* fontData, const uint32_t glyphIndex, StreamInfo& info) {
  const EpdGlyph& glyph = fontData->glyph[glyphIndex];
  if (glyph.width == 0 || glyph.height == 0) {
    if (glyph.dataLength != 0) {
      LOG_ERR("GLYPH", "Empty glyph %u has %u stream bytes", glyphIndex, glyph.dataLength);
      return false;
    }
    return true;
  }
  if (glyph.dataLength == 0) {
    LOG_ERR("GLYPH", "Glyph %u has no stream header", glyphIndex);
    return false;
  }

  const uint8_t* stream = fontData->bitmap + glyph.dataOffset;
  const uint8_t header = stream[0];
  if ((header & RESERVED_MASK) != 0) {
    LOG_ERR("GLYPH", "Glyph %u uses reserved header bits", glyphIndex);
    return false;
  }

  info.raw = (header & RAW_FLAG) != 0;
  info.hasReference = (header & HAS_REF_FLAG) != 0;
  if (info.raw) {
    if (header != RAW_FLAG) {
      LOG_ERR("GLYPH", "RAW glyph %u has invalid header", glyphIndex);
      return false;
    }
    const size_t required = GlyphStreamCodec::packedSize(glyph.width, glyph.height, fontData->is2Bit);
    if (glyph.dataLength < 1 + required) {
      LOG_ERR("GLYPH", "RAW glyph %u is truncated", glyphIndex);
      return false;
    }
    info.payload = stream + 1;
    info.payloadSize = glyph.dataLength - 1;
    return true;
  }

  if (info.hasReference) {
    if (glyph.dataLength < 3) {
      LOG_ERR("GLYPH", "Referenced glyph %u has no base index", glyphIndex);
      return false;
    }
    info.baseIndex = static_cast<uint16_t>(stream[1]) | (static_cast<uint16_t>(stream[2]) << 8);
    info.shift = static_cast<int8_t>(header & 0x0F) - SHIFT_BIAS;
    info.payload = stream + 3;
    info.payloadSize = glyph.dataLength - 3;
  } else {
    if ((header & 0x0F) != 0) {
      LOG_ERR("GLYPH", "Unreferenced glyph %u has a shift", glyphIndex);
      return false;
    }
    info.payload = stream + 1;
    info.payloadSize = glyph.dataLength - 1;
  }
  return true;
}

#if defined(__GNUC__) || defined(__clang__)
#define GLYPH_STREAM_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define GLYPH_STREAM_ALWAYS_INLINE inline
#endif

GLYPH_STREAM_ALWAYS_INLINE uint8_t pixelAt(const uint8_t* plane, const uint8_t width, const uint8_t height, const int x,
                                           const int y) {
  if (plane == nullptr || x < 0 || y < 0 || x >= width || y >= height) return 0;
  return plane[static_cast<size_t>(y) * width + x];
}

GLYPH_STREAM_ALWAYS_INLINE uint8_t basePixelAt(const uint8_t* plane, const uint8_t width, const uint8_t height,
                                               const int x, const int y, const int8_t shift) {
  return pixelAt(plane, width, height, x, y + shift);
}

#undef GLYPH_STREAM_ALWAYS_INLINE

uint16_t treeProbability(const GlyphStreamTreeNode* nodes, const uint16_t nodeCount, const uint16_t* probabilities,
                         const uint8_t* features) {
  if (nodeCount == 0) return probabilities[0];
  int16_t entry = 0;
  while (entry >= 0) {
    const GlyphStreamTreeNode& node = nodes[entry];
    entry = features[node.feature] ? node.child1 : node.child0;
  }
  return probabilities[static_cast<uint16_t>(~entry)];
}

void inkFeatures(const uint8_t* plane, const EpdGlyph& glyph, const int x, const int y, const uint8_t* basePlane,
                 const uint8_t baseHeight, const int8_t shift, uint8_t* features) {
  features[0] = pixelAt(plane, glyph.width, glyph.height, x - 1, y) > 0;
  features[1] = pixelAt(plane, glyph.width, glyph.height, x, y - 1) > 0;
  features[2] = pixelAt(plane, glyph.width, glyph.height, x - 1, y - 1) > 0;
  features[3] = pixelAt(plane, glyph.width, glyph.height, x + 1, y - 1) > 0;
  features[4] = pixelAt(plane, glyph.width, glyph.height, x - 2, y) > 0;
  features[5] = pixelAt(plane, glyph.width, glyph.height, x, y - 2) > 0;
  features[6] = pixelAt(plane, glyph.width, glyph.height, x - 1, y - 2) > 0;
  features[7] = pixelAt(plane, glyph.width, glyph.height, x + 1, y - 2) > 0;
  features[8] = pixelAt(plane, glyph.width, glyph.height, x - 3, y) > 0;
  features[9] = pixelAt(plane, glyph.width, glyph.height, x + 2, y - 1) > 0;
  features[10] = pixelAt(plane, glyph.width, glyph.height, x + 2, y - 2) > 0;
  features[11] = pixelAt(plane, glyph.width, glyph.height, x - 2, y - 1) > 0;
  features[12] = 4 * x < glyph.width;
  features[13] = 4 * x >= 3 * glyph.width;
  features[14] = 4 * y < glyph.height;
  features[15] = 4 * y >= 3 * glyph.height;
  features[16] = basePixelAt(basePlane, glyph.width, baseHeight, x, y, shift) > 0;
  features[17] = basePixelAt(basePlane, glyph.width, baseHeight, x, y + 1, shift) > 0;
}

void grayFeatures(const uint8_t* plane, const EpdGlyph& glyph, const int x, const int y, const uint8_t* basePlane,
                  const uint8_t baseHeight, const int8_t shift, uint8_t* features) {
  const uint8_t west = pixelAt(plane, glyph.width, glyph.height, x - 1, y);
  const uint8_t north = pixelAt(plane, glyph.width, glyph.height, x, y - 1);
  const uint8_t northeast = pixelAt(plane, glyph.width, glyph.height, x + 1, y - 1);
  const uint8_t northwest = pixelAt(plane, glyph.width, glyph.height, x - 1, y - 1);
  const uint8_t base = basePixelAt(basePlane, glyph.width, baseHeight, x, y, shift);
  features[0] = west & 1;
  features[1] = (west >> 1) & 1;
  features[2] = north & 1;
  features[3] = (north >> 1) & 1;
  features[4] = northeast & 1;
  features[5] = (northeast >> 1) & 1;
  features[6] = northwest & 1;
  features[7] = (northwest >> 1) & 1;
  features[8] = pixelAt(plane, glyph.width, glyph.height, x, y + 1) > 0;
  features[9] = pixelAt(plane, glyph.width, glyph.height, x - 1, y + 1) > 0;
  features[10] = pixelAt(plane, glyph.width, glyph.height, x + 1, y + 1) > 0;
  features[11] = base & 1;
  features[12] = (base >> 1) & 1;
  features[13] = 4 * x < glyph.width;
  features[14] = 4 * x >= 3 * glyph.width;
  features[15] = 4 * y < glyph.height;
  features[16] = 4 * y >= 3 * glyph.height;
}

uint8_t unpackedPixel(const uint8_t* packed, const size_t pixelIndex, const bool is2Bit) {
  const uint8_t bitsPerPixel = is2Bit ? 2 : 1;
  const size_t bitOffset = pixelIndex * bitsPerPixel;
  const uint8_t shift = 8 - bitsPerPixel - (bitOffset & 7);
  return (packed[bitOffset >> 3] >> shift) & (is2Bit ? 0x03 : 0x01);
}

bool decodeToPlane(const EpdFontData* fontData, const uint32_t glyphIndex, const StreamInfo& info,
                   const uint8_t* basePlane, const uint8_t baseHeight, const bool wantGray, uint8_t* plane) {
  const EpdGlyph& glyph = fontData->glyph[glyphIndex];
  const size_t area = static_cast<size_t>(glyph.width) * glyph.height;
  if (info.raw) {
    for (size_t pixelIndex = 0; pixelIndex < area; ++pixelIndex) {
      const uint8_t value = unpackedPixel(info.payload, pixelIndex, fontData->is2Bit);
      plane[pixelIndex] = !wantGray && value > 0 ? 1 : value;
    }
    return true;
  }

  std::memset(plane, 0, area);
  RangeDecoder decoder(info.payload, info.payloadSize);
  uint8_t features[18] = {};
  for (uint8_t y = 0; y < glyph.height; ++y) {
    for (uint8_t x = 0; x < glyph.width; ++x) {
      inkFeatures(plane, glyph, x, y, basePlane, baseHeight, info.shift, features);
      const uint16_t probability = treeProbability(kInkTree, GLYPH_STREAM_INK_NODE_COUNT, kInkProbs, features);
      plane[static_cast<size_t>(y) * glyph.width + x] = decoder.decodeBit(probability);
    }
  }

  if (!fontData->is2Bit || !wantGray) return true;
  for (uint8_t y = 0; y < glyph.height; ++y) {
    for (uint8_t x = 0; x < glyph.width; ++x) {
      const size_t pixelIndex = static_cast<size_t>(y) * glyph.width + x;
      if (plane[pixelIndex] == 0) continue;
      grayFeatures(plane, glyph, x, y, basePlane, baseHeight, info.shift, features);
      const uint8_t isBlack =
          decoder.decodeBit(treeProbability(kHiTree, GLYPH_STREAM_HI_NODE_COUNT, kHiProbs, features));
      if (isBlack) {
        plane[pixelIndex] = 3;
      } else {
        const uint8_t isDark =
            decoder.decodeBit(treeProbability(kLoTree, GLYPH_STREAM_LO_NODE_COUNT, kLoProbs, features));
        plane[pixelIndex] = isDark ? 2 : 1;
      }
    }
  }
  return true;
}

void packPlane(const uint8_t* plane, const size_t pixelCount, const bool is2Bit, uint8_t* output,
               const size_t outputSize) {
  std::memset(output, 0, outputSize);
  const uint8_t bitsPerPixel = is2Bit ? 2 : 1;
  const uint8_t mask = is2Bit ? 0x03 : 0x01;
  for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
    const size_t bitOffset = pixelIndex * bitsPerPixel;
    const uint8_t shift = 8 - bitsPerPixel - (bitOffset & 7);
    output[bitOffset >> 3] |= (plane[pixelIndex] & mask) << shift;
  }
}

}  // namespace

size_t GlyphStreamCodec::packedSize(const uint8_t width, const uint8_t height, const bool is2Bit) {
  const size_t bits = static_cast<size_t>(width) * height * (is2Bit ? 2 : 1);
  return (bits + 7) / 8;
}

bool GlyphStreamCodec::decode(const EpdFontData* fontData, const uint32_t glyphIndex, const bool wantGray,
                              uint8_t* scratchA, uint8_t* scratchB, uint8_t* output, const size_t outputCapacity) {
  if (fontData == nullptr || fontData->bitmap == nullptr || fontData->glyph == nullptr || scratchA == nullptr ||
      scratchB == nullptr || (output == nullptr && outputCapacity != 0)) {
    LOG_ERR("GLYPH", "Invalid GlyphStream decode arguments");
    return false;
  }
  const uint32_t count = glyphCount(fontData);
  if (glyphIndex >= count) {
    LOG_ERR("GLYPH", "Glyph index %u is out of range", glyphIndex);
    return false;
  }

  const EpdGlyph& target = fontData->glyph[glyphIndex];
  if (!dimensionsFit(target)) {
    LOG_ERR("GLYPH", "Glyph %u dimensions %ux%u exceed scratch planes", glyphIndex, target.width, target.height);
    return false;
  }
  const size_t outputSize = packedSize(target.width, target.height, fontData->is2Bit);
  if (outputCapacity < outputSize) {
    LOG_ERR("GLYPH", "Glyph %u output buffer is too small", glyphIndex);
    return false;
  }
  if (target.width == 0 || target.height == 0) {
    if (target.dataLength != 0) {
      LOG_ERR("GLYPH", "Empty glyph %u has stream bytes", glyphIndex);
      return false;
    }
    return true;
  }

  uint32_t chain[3] = {glyphIndex, 0, 0};
  StreamInfo streams[3] = {};
  uint8_t chainLength = 1;
  for (uint8_t chainIndex = 0; chainIndex < chainLength; ++chainIndex) {
    const uint32_t currentIndex = chain[chainIndex];
    if (!dimensionsFit(fontData->glyph[currentIndex]) || !parseStream(fontData, currentIndex, streams[chainIndex])) {
      return false;
    }
    if (!streams[chainIndex].hasReference) continue;
    if (chainLength == 3) {
      LOG_ERR("GLYPH", "Glyph %u reference chain exceeds depth two", glyphIndex);
      return false;
    }
    const uint16_t baseIndex = streams[chainIndex].baseIndex;
    if (baseIndex >= currentIndex) {
      LOG_ERR("GLYPH", "Glyph %u base %u is not earlier", currentIndex, baseIndex);
      return false;
    }
    if (fontData->glyph[baseIndex].width != fontData->glyph[currentIndex].width) {
      LOG_ERR("GLYPH", "Glyph %u base width differs", currentIndex);
      return false;
    }
    chain[chainLength++] = baseIndex;
  }

  for (int chainIndex = chainLength - 1; chainIndex >= 0; --chainIndex) {
    uint8_t* destination = (chainIndex & 1) ? scratchB : scratchA;
    const uint8_t* basePlane = nullptr;
    uint8_t baseHeight = 0;
    if (streams[chainIndex].hasReference) {
      basePlane = ((chainIndex + 1) & 1) ? scratchB : scratchA;
      baseHeight = fontData->glyph[chain[chainIndex + 1]].height;
    }
    if (!decodeToPlane(fontData, chain[chainIndex], streams[chainIndex], basePlane, baseHeight, wantGray,
                       destination)) {
      return false;
    }
  }

  packPlane(scratchA, static_cast<size_t>(target.width) * target.height, fontData->is2Bit, output, outputSize);
  return true;
}
