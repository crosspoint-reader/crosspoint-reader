#include "ImageOpsConfig.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <cstring>

#include "ImageOps.h"

#if USE_SIMD_S3_PIE
namespace {

#define REPEAT_FOUR(a, b, c, d) {a, b, c, d, a, b, c, d, a, b, c, d, a, b, c, d}
alignas(16) constexpr int8_t OFFSETS[4][4][16] = {{REPEAT_FOUR(-40, 0, -30, 10), REPEAT_FOUR(0, -30, 10, -40),
                                                   REPEAT_FOUR(-30, 10, -40, 0), REPEAT_FOUR(10, -40, 0, -30)},
                                                  {REPEAT_FOUR(20, -20, 30, -10), REPEAT_FOUR(-20, 30, -10, 20),
                                                   REPEAT_FOUR(30, -10, 20, -20), REPEAT_FOUR(-10, 20, -20, 30)},
                                                  {REPEAT_FOUR(-25, 15, -35, 5), REPEAT_FOUR(15, -35, 5, -25),
                                                   REPEAT_FOUR(-35, 5, -25, 15), REPEAT_FOUR(5, -25, 15, -35)},
                                                  {REPEAT_FOUR(35, -5, 25, -15), REPEAT_FOUR(-5, 25, -15, 35),
                                                   REPEAT_FOUR(25, -15, 35, -5), REPEAT_FOUR(-15, 35, -5, 25)}};
#undef REPEAT_FOUR

alignas(16) constexpr uint8_t CONSTANTS[32] = {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
                                               128, 128, 128, 128, 128, 3,   3,   3,   3,   3,   3,
                                               3,   3,   3,   3,   3,   3,   3,   3,   3,   3};

}  // namespace

extern "C" void imageOpsBayerSpanS3Blocks(const uint8_t* input, uint8_t* output, size_t blocks, const int8_t* offsets,
                                          const uint8_t* constants);
#endif

namespace imageops {
namespace {

void scalarSpan(const uint8_t* input, uint8_t* output, size_t count, int x, int y) {
  static constexpr uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const auto* row = BAYER[static_cast<unsigned>(y) & 3];
  const unsigned phase = static_cast<unsigned>(x) & 3;
  for (size_t i = 0; i < count; ++i) {
    int adjusted = input[i] + (row[(phase + (i & 3)) & 3] - 8) * 5;
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;
    output[i] = static_cast<uint8_t>(adjusted >> 6);
  }
}

}  // namespace

void ditherBayer4LevelSpan(const uint8_t* input, uint8_t* output, size_t count, int x, int y) {
  if (count == 0) return;
#if USE_SIMD_S3_PIE
  const auto* offsets = OFFSETS[static_cast<unsigned>(y) & 3][static_cast<unsigned>(x) & 3];
  const size_t vectorCount = count & ~size_t{15};
  if (((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output)) & 15) == 0) {
    if (vectorCount != 0) imageOpsBayerSpanS3Blocks(input, output, vectorCount / 16, offsets, CONSTANTS);
  } else {
    // PIE vector addresses round down; stage only bytes within the valid span.
    alignas(16) uint8_t inputBlock[64];
    alignas(16) uint8_t outputBlock[64];
    for (size_t i = 0; i < vectorCount;) {
      const size_t bytes = vectorCount - i < sizeof(inputBlock) ? vectorCount - i : sizeof(inputBlock);
      memcpy(inputBlock, input + i, bytes);
      imageOpsBayerSpanS3Blocks(inputBlock, outputBlock, bytes / 16, offsets, CONSTANTS);
      memcpy(output + i, outputBlock, bytes);
      i += bytes;
    }
  }
  // vectorCount is a multiple of the four-pixel Bayer period.
  scalarSpan(input + vectorCount, output + vectorCount, count - vectorCount, x, y);
#else
  scalarSpan(input, output, count, x, y);
#endif
}

}  // namespace imageops
#endif
