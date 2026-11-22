#include "ImageOpsConfig.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <cstring>

#include "ImageOps.h"

#if USE_SIMD_S3_PIE
extern "C" void imageOpsRgb24GrayS3Blocks(const uint8_t* input, uint8_t* output, size_t blocks, const int16_t* weights);

namespace {
alignas(16) constexpr int16_t RGB_WEIGHTS[] = {77,   77,   77,   77,   77,   77, 77, 77, -106, -106, -106,
                                               -106, -106, -106, -106, -106, 29, 29, 29, 29,   29,   29,
                                               29,   29,   1,    1,    1,    1,  1,  1,  1,    1};
alignas(16) constexpr int16_t BGR_WEIGHTS[] = {29,   29,   29,   29,   29,   29, 29, 29, -106, -106, -106,
                                               -106, -106, -106, -106, -106, 77, 77, 77, 77,   77,   77,
                                               77,   77,   1,    1,    1,    1,  1,  1,  1,    1};

// count is a positive multiple of eight pixels.
void convertBlocks(const uint8_t* input, uint8_t* output, size_t count, const int16_t* weights) {
  if (((reinterpret_cast<uintptr_t>(input) | reinterpret_cast<uintptr_t>(output)) & 3) == 0) {
    imageOpsRgb24GrayS3Blocks(input, output, count / 8, weights);
  } else {
    alignas(16) uint8_t packed[48];
    alignas(16) uint8_t gray[16];
    for (size_t x = 0; x < count;) {
      const size_t pixels = count - x < sizeof(gray) ? count - x : sizeof(gray);
      memcpy(packed, input + x * 3, pixels * 3);
      imageOpsRgb24GrayS3Blocks(packed, gray, pixels / 8, weights);
      memcpy(output + x, gray, pixels);
      x += pixels;
    }
  }
}
}  // namespace
#endif

namespace imageops {

void rgb24ToGraySpan(const uint8_t* input, uint8_t* output, size_t count) {
  if (count == 0) return;
#if USE_SIMD_S3_PIE
  const size_t vectorCount = count & ~size_t{7};
  if (vectorCount) convertBlocks(input, output, vectorCount, RGB_WEIGHTS);
  input += vectorCount * 3;
  output += vectorCount;
  count -= vectorCount;
#endif
  for (size_t x = 0; x < count; ++x) {
    output[x] = static_cast<uint8_t>((77u * input[x * 3] + 150u * input[x * 3 + 1] + 29u * input[x * 3 + 2]) >> 8);
  }
}

void bgr24ToGraySpan(const uint8_t* input, uint8_t* output, size_t count) {
  if (count == 0) return;
#if USE_SIMD_S3_PIE
  const size_t vectorCount = count & ~size_t{7};
  if (vectorCount) convertBlocks(input, output, vectorCount, BGR_WEIGHTS);
  input += vectorCount * 3;
  output += vectorCount;
  count -= vectorCount;
#endif
  for (size_t x = 0; x < count; ++x) {
    output[x] = static_cast<uint8_t>((29u * input[x * 3] + 150u * input[x * 3 + 1] + 77u * input[x * 3 + 2]) >> 8);
  }
}

}  // namespace imageops
#endif
