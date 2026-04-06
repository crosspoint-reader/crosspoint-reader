#pragma once

#include <stdint.h>

#include <cstring>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

inline uint8_t quantizeGray4Level(uint8_t gray) { return gray >= 213 ? 3 : static_cast<uint8_t>((gray + 42) / 85); }

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  return quantizeGray4Level((uint8_t)adjusted);
}

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
class DiffusedBayerDitherer {
 public:
  explicit DiffusedBayerDitherer(int width) : width(width) {
    errorCurRow = new int16_t[width + 2]();
    errorNextRow = new int16_t[width + 2]();
  }

  ~DiffusedBayerDitherer() {
    delete[] errorCurRow;
    delete[] errorNextRow;
  }

  DiffusedBayerDitherer(const DiffusedBayerDitherer&) = delete;
  DiffusedBayerDitherer& operator=(const DiffusedBayerDitherer&) = delete;

  uint8_t processPixel(int gray, int x, int screenX, int screenY) {
    int adjusted = gray + errorCurRow[x + 1];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    int thresholdAdjusted = adjusted + (bayer4x4[screenY & 3][screenX & 3] - 8) * 5;
    if (thresholdAdjusted < 0) thresholdAdjusted = 0;
    if (thresholdAdjusted > 255) thresholdAdjusted = 255;

    uint8_t quantized = quantizeGray4Level((uint8_t)thresholdAdjusted);
    int quantizedValue = quantized * 85;
    int error = adjusted - quantizedValue;

    errorCurRow[x + 2] += (error * 7) / 16;
    errorNextRow[x] += (error * 3) / 16;
    errorNextRow[x + 1] += (error * 5) / 16;
    errorNextRow[x + 2] += error / 16;

    return quantized;
  }

  void nextRow() {
    int16_t* tmp = errorCurRow;
    errorCurRow = errorNextRow;
    errorNextRow = tmp;
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
  }

  void reset() {
    memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
  }

 private:
  int width;
  int16_t* errorCurRow;
  int16_t* errorNextRow;
};
#endif
