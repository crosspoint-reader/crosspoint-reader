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
  // differential LUT. Apply a -8 offset to mid-bright pixels onward to bring
  // highlights/midtones back down without crushing deep shadow detail.
  // Ramp the offset from 0 to 8 across gray [0, 64], flat -8 above 64.
  int g = gray;
  int offset = (g < 64) ? g * 8 / 64 : 8;
  g -= offset;

  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = g + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  // T01=48: slightly above original 43, keeps shadow detail while allowing
  // near-black pixels to lift to dark gray.
  // T12=133: raised from 128 to push more mid-bright source pixels into the
  // palette 1 / palette 2 dither zone for perceptual mid-gray.
  // T23=218: raised from 192 to expand light-gray range, preserving
  // highlight detail on the brighter factory LUT.
  if (adjusted < 48) return 0;
  if (adjusted < 133) return 1;
  if (adjusted < 218) return 2;
  return 3;
}
