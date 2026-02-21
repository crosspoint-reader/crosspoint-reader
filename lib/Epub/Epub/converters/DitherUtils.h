#pragma once

#include <GfxRenderer.h>
#include <stdint.h>

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3).
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

// Apply deterministic spatial-noise dithering and quantize to 4 levels (0-3).
// This avoids the visible "grid" artifact of ordered Bayer dithering.
inline uint8_t applyNoiseDither4Level(uint8_t gray, int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u;
  h ^= static_cast<uint32_t>(y) * 668265263u;
  h ^= (h >> 13);
  h *= 1274126177u;
  h ^= (h >> 16);

  // Map low byte to [-32, +31], gentler than Bayer's +/-40.
  int dither = static_cast<int>(h & 0xFFu) - 128;
  dither >>= 2;

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Draw a pixel respecting the current render mode for grayscale support
inline void drawPixelWithRenderMode(GfxRenderer& renderer, int x, int y, uint8_t pixelValue) {
  GfxRenderer::RenderMode renderMode = renderer.getRenderMode();
  if (renderMode == GfxRenderer::BW && pixelValue < 3) {
    renderer.drawPixel(x, y, true);
  } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (pixelValue == 1 || pixelValue == 2)) {
    renderer.drawPixel(x, y, false);
  } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && pixelValue == 1) {
    renderer.drawPixel(x, y, false);
  }
}
