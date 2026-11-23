#include "ImageOpsConfig.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <cstring>

#include "ImageOps.h"

#if USE_SIMD_S3_PIE
namespace {

alignas(16) constexpr uint32_t LEVEL_MASKS[] = {0x000F000F, 0x000F000F, 0x000F000F, 0x000F000F, 0x000000FF, 0x000000FF,
                                                0x000000FF, 0x000000FF, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
                                                0x33333333, 0x33333333, 0x33333333, 0x33333333, 0x0F0F0F0F, 0x0F0F0F0F,
                                                0x0F0F0F0F, 0x0F0F0F0F, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF};
alignas(16) constexpr uint32_t PACKED_MASKS[] = {0x00330033, 0x00330033, 0x00330033, 0x00330033, 0x00000F0F,
                                                 0x00000F0F, 0x00000F0F, 0x00000F0F, 0x55555555, 0x55555555,
                                                 0x55555555, 0x55555555, 0x33333333, 0x33333333, 0x33333333,
                                                 0x33333333, 0x0F0F0F0F, 0x0F0F0F0F, 0x0F0F0F0F, 0x0F0F0F0F};

}  // namespace

extern "C" void imageOpsLevelTilePlanesS3(const uint8_t* levels, uint8_t* planes, const uint32_t* masks);
extern "C" void imageOpsPackedTilePlanesS3(const uint8_t* input, uint8_t* planes, const uint32_t* masks);
#endif

namespace imageops {

void levelTile8x8ToPlanes(const uint8_t* levels, uint8_t* planes) {
#if USE_SIMD_S3_PIE
  alignas(16) uint8_t input[64];
  alignas(16) uint8_t output[16];
  const uint8_t* source = levels;
  if ((reinterpret_cast<uintptr_t>(levels) & 15) != 0) {
    memcpy(input, levels, sizeof(input));
    source = input;
  }
  uint8_t* destination = (reinterpret_cast<uintptr_t>(planes) & 15) == 0 ? planes : output;
  imageOpsLevelTilePlanesS3(source, destination, LEVEL_MASKS);
  if (destination != planes) memcpy(planes, output, sizeof(output));
#else
  for (int column = 0; column < 8; ++column) {
    uint8_t low = 0, high = 0;
    for (int row = 0; row < 8; ++row) {
      const uint8_t level = levels[row * 8 + column];
      low |= (level & 1) << (7 - row);
      high |= (level >> 1) << (7 - row);
    }
    planes[column] = low;
    planes[8 + column] = high;
  }
#endif
}

void packed2BitTile8x8ToPlanes(const uint8_t* const rows[8], uint8_t* planes) {
#if USE_SIMD_S3_PIE
  // Group the first and second packed bytes of eight independent rows.
  alignas(16) uint8_t input[16];
  alignas(16) uint8_t output[16];
  for (int row = 0; row < 8; ++row) {
    input[row] = rows[row][0];
    input[8 + row] = rows[row][1];
  }
  uint8_t* destination = (reinterpret_cast<uintptr_t>(planes) & 15) == 0 ? planes : output;
  imageOpsPackedTilePlanesS3(input, destination, PACKED_MASKS);
  if (destination != planes) memcpy(planes, output, sizeof(output));
#else
  for (int column = 0; column < 8; ++column) {
    uint8_t low = 0, high = 0;
    for (int row = 0; row < 8; ++row) {
      const uint8_t level = (rows[row][column / 4] >> (6 - (column & 3) * 2)) & 3;
      low |= (level & 1) << (7 - row);
      high |= (level >> 1) << (7 - row);
    }
    planes[column] = low;
    planes[8 + column] = high;
  }
#endif
}

}  // namespace imageops
#endif
