#pragma once

#include <BlueNoise64.h>
#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Quantize an 8-bit gray to the 2-bit value the pixel writers consume.
// - ditherBw (Fast mode): blue-noise threshold straight from the 8-bit gray to a
//   pure BW value (0 = ink, 3 = white). A single ordered pattern with full tonal
//   range — avoids stacking blue-noise on top of an already-Bayer'd 4-level value.
// - else useDithering: Bayer 4-level (the Quality grayscale path).
// - else: plain nearest 4-level.
inline uint8_t quantizeGrayTo2Bit(uint8_t gray, int x, int y, bool useDithering, bool ditherBw) {
  if (ditherBw) {
    return gray < BLUE_NOISE_64[y & 63][x & 63] ? 0 : 3;
  }
  if (useDithering) {
    return applyBayerDither4Level(gray, x, y);
  }
  uint8_t v = gray / 85;
  return v > 3 ? 3 : v;
}
