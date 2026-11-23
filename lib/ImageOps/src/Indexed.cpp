#include "ImageOpsConfig.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <cstring>

#include "ImageOps.h"

#if USE_SIMD_S3_PIE
extern "C" void imageOpsUnpackIndex1S3(const uint8_t*, uint8_t*, size_t, const uint8_t*);
extern "C" void imageOpsUnpackIndex2S3(const uint8_t*, uint8_t*, size_t, const uint8_t*);
extern "C" void imageOpsUnpackIndex4S3(const uint8_t*, uint8_t*, size_t, const uint8_t*);
namespace {
alignas(16) constexpr uint8_t MASK1[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
alignas(16) constexpr uint8_t MASK2[16] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
alignas(16) constexpr uint8_t MASK4[16] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15};
}  // namespace
#endif

namespace imageops {
namespace {
uint8_t readIndex(const uint8_t* input, size_t first, unsigned bits) {
  const unsigned perByte = 8 / bits;
  return (input[first / perByte] >> (8 - bits - (first % perByte) * bits)) & ((1u << bits) - 1);
}

void unpackScalar(const uint8_t* input, uint8_t* output, size_t count, IndexDepth depth) {
  switch (depth) {
    case IndexDepth::Bits1:
      while (count >= 8) {
        const uint8_t value = *input++;
        output[0] = value >> 7;
        output[1] = (value >> 6) & 1;
        output[2] = (value >> 5) & 1;
        output[3] = (value >> 4) & 1;
        output[4] = (value >> 3) & 1;
        output[5] = (value >> 2) & 1;
        output[6] = (value >> 1) & 1;
        output[7] = value & 1;
        output += 8;
        count -= 8;
      }
      break;
    case IndexDepth::Bits2:
      while (count >= 4) {
        const uint8_t value = *input++;
        output[0] = value >> 6;
        output[1] = (value >> 4) & 3;
        output[2] = (value >> 2) & 3;
        output[3] = value & 3;
        output += 4;
        count -= 4;
      }
      break;
    case IndexDepth::Bits4:
      while (count >= 2) {
        const uint8_t value = *input++;
        output[0] = value >> 4;
        output[1] = value & 15;
        output += 2;
        count -= 2;
      }
      break;
    case IndexDepth::Bits8:
      memcpy(output, input, count);
      return;
  }
  for (size_t x = 0; x < count; ++x) output[x] = readIndex(input, x, static_cast<unsigned>(depth));
}
}  // namespace

void unpackIndexSpan(const uint8_t* input, uint8_t* output, size_t count, IndexDepth depth, size_t first) {
  if (count == 0) return;
  if (depth == IndexDepth::Bits8) {
    memcpy(output, input + first, count);
    return;
  }
  const unsigned bits = static_cast<unsigned>(depth);
  const unsigned perByte = 8 / bits;
  while (count && first % perByte) {
    *output++ = readIndex(input, first++, bits);
    --count;
  }
  input += first / perByte;
#if USE_SIMD_S3_PIE
  using UnpackBlocks = void (*)(const uint8_t*, uint8_t*, size_t, const uint8_t*);
  const uint8_t* mask = nullptr;
  UnpackBlocks kernel = nullptr;
  switch (depth) {
    case IndexDepth::Bits1:
      mask = MASK1;
      kernel = imageOpsUnpackIndex1S3;
      break;
    case IndexDepth::Bits2:
      mask = MASK2;
      kernel = imageOpsUnpackIndex2S3;
      break;
    case IndexDepth::Bits4:
      mask = MASK4;
      kernel = imageOpsUnpackIndex4S3;
      break;
    case IndexDepth::Bits8:
      return;
  }
  const size_t perBlock = perByte * 16;
  if (((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output)) & 15) == 0) {
    const size_t blocks = count / perBlock;
    if (blocks != 0) kernel(input, output, blocks, mask);
    input += blocks * 16;
    output += blocks * perBlock;
    count -= blocks * perBlock;
  } else {
    alignas(16) uint8_t packed[16], expanded[128];
    while (count >= perBlock) {
      memcpy(packed, input, sizeof(packed));
      kernel(packed, expanded, 1, mask);
      memcpy(output, expanded, perBlock);
      input += 16;
      output += perBlock;
      count -= perBlock;
    }
  }
#endif
  unpackScalar(input, output, count, depth);
}

void prepareIndexedPalette(const uint8_t* palette, IndexDepth depth, bool hasAlpha, bool retainAlpha, uint8_t* gray,
                           uint8_t* alpha) {
  const unsigned entries = 1u << static_cast<unsigned>(depth);
  for (unsigned index = 0; index < entries; ++index) {
    unsigned value, a = 255;
    if (palette) {
      const auto* rgb = palette + index * 3;
      value = (77u * rgb[0] + 150u * rgb[1] + 29u * rgb[2]) >> 8;
      if (hasAlpha) a = palette[768 + index];
    } else {
      value = index * 255u / (entries - 1);
    }
    gray[index] = static_cast<uint8_t>(retainAlpha ? value : (value * a + 255u * (255u - a)) / 255u);
    alpha[index] = static_cast<uint8_t>(a);
  }
}

}  // namespace imageops
#endif
