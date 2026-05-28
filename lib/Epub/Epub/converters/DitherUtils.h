#pragma once

#include <BlueNoise64.h>
#include <stdint.h>

// 4-level dither (0-3) for the JPEG/PNG converters writing into the grayscale
// planes (AA-on path: Sleep image, XTC, EpubReader with AA on). Stateless:
// any pixel iteration order produces the same output.
//
// The 64x64 blue-noise table places dither texture above the eye's
// contrast-sensitivity peak so smooth gradients read as fine grain rather than
// the regular crosshatch a 4x4 Bayer matrix would produce. Perturbation
// magnitude is +/-42, roughly half the 85-step quantization quantum, so a
// pixel within one quantum of a boundary can shift into the neighbor level.
inline uint8_t applyBlueNoiseDither4Level(uint8_t gray, int x, int y) {
  const int noise = BLUE_NOISE_64[y & 63][x & 63];  // 0..255
  // Perturbation in [-43, 42]: ((noise - 128) * 85) >> 8 ~= (noise - 128) / 3.
  // Hand-rolled to avoid the per-pixel DIV (~35 cycles on RV32IM); a full
  // 800x480 decode hits this 384k times so the shift form saves ~80ms.
  const int dither = ((noise - 128) * 85) >> 8;
  int adjusted = static_cast<int>(gray) + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}
