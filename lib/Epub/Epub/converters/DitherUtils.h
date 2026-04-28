#pragma once

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
  // Soft-shoulder darkening for factory LUT: EPUB image pages render on the
  // factory LUT, where palette levels are physically lighter than on the
  // differential LUT. Apply a -12 offset to mid-bright pixels onward to bring
  // highlights/midtones back down without crushing deep shadow detail.
  // Ramp the offset from 0 to 12 across gray [0, 64], flat -12 above 64.
  int g = gray;
  int offset = (g < 64) ? g * 12 / 64 : 12;
  g -= offset;

  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = g + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  // T12 raised from 128 to 150 so mid-bright source pixels (sRGB 150–170)
  // land in the palette 1 / palette 2 dither zone, producing ~50% perceived
  // reflectance via 57/43 mixing — the perceptual mid-gray that factory LUT
  // can't reach with palette 2 alone (~70% reflectance).
  // T23 raised from 192 to 210 to keep mid-bright pixels (sRGB 180–210) from
  // blowing out to pure white after the soft-shoulder offset is applied.
  if (adjusted < 64) return 0;
  if (adjusted < 150) return 1;
  if (adjusted < 210) return 2;
  return 3;
}
