#pragma once

// Glyph resampling for text drawn at a scale (GfxRenderer's renderCharAtScale): pure, so the host tests
// include it directly.
#include <algorithm>
#include <cstdint>

// Resample one glyph to `scale`, reporting every destination pixel that must be drawn to
// `emit(dstX, dstY)`. Single source of truth for both consumers: the cache builder (which
// records the pixels into a 1-bit mask) and the uncached fallback (which draws them
// straight to the framebuffer). Keeping one copy of the maths is what stops the two from
// drifting apart — the cached and uncached paths must produce identical pixels.
//
// `drawMask` picks the ink levels that are emitted (bit n: level n), at every scale.
template <typename Emit>
static inline void emitScaledGlyphPixels(const uint8_t* const bitmap, const bool is2Bit, const int srcW, const int srcH,
                                         const int dstW, const int dstH, const float scale, const uint8_t drawMask,
                                         Emit&& emit) {
  // The ESP32-C3 has no FPU — every float op in a per-pixel loop is a soft-float call
  // costing hundreds of cycles. The resampling therefore runs in 16.16 fixed point:
  // the ONLY float operation per glyph is the one-time conversion of `scale` into a
  // fixed-point step; the pixel loops are pure integer (the C3's M extension gives a
  // hardware divider for the one division per destination pixel).
  constexpr int FP_SHIFT = 16;
  constexpr int32_t FP_ONE = 1 << FP_SHIFT;
  const int32_t invScaleFP = static_cast<int32_t>(FP_ONE / scale + 0.5f);

  // Per-source-pixel raw ink level: 2-bit fonts carry 4 AA levels (0..3); 1-bit fonts map
  // to 0 or 3.
  auto srcRaw = [&](const int sx, const int sy) -> uint8_t {
    const int pos = sy * srcW + sx;
    if (is2Bit) {
      const uint8_t byte = bitmap[pos >> 2];
      return (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
    }
    const uint8_t byte = bitmap[pos >> 3];
    return ((byte >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

  // Area-weighted coverage resampling, enlarging and shrinking alike, which keeps the font's grayscale
  // AA. Nearest-neighbour sampling bloated each source pixel into a scale×scale solid block when
  // enlarging, so light-gray AA edge pixels (raw==1, 33% coverage) rendered as full-black blocks — the
  // jagged, over-bold look. When shrinking it skipped whole source rows and columns (every fourth at
  // 0.75), so a one-pixel stroke could vanish, stems came out uneven and no edge was gray.
  //
  // Here each destination pixel integrates the fractional source coverage over the source region it
  // maps back to (0..1), then re-quantizes that coverage into a 2-bit AA level (0=white .. 3=black).
  // That reconstructed level feeds the SAME multi-pass machinery body text uses: drawMaskFor2BitMode
  // decides, per render pass and darkness setting, whether this level draws into the current plane.
  // In grayscale render modes scaled text therefore emits true gray edge pixels (MSB/LSB planes)
  // instead of a hard 1-bit threshold; in BW mode every non-white level draws, matching the body
  // font's weight. (The caller derives the pixel state from the render mode: grayscale passes clear
  // the bit, only the BW pass honors the caller's pixelState.)
  //
  // Area integration in fixed point. Overlap extents are 16.16 and at most one source pixel
  // (FP_ONE) each; the raw x h x w product needs 64 bits, which
  // RV32IM handles with a few integer multiplies — still an order of magnitude cheaper
  // than one soft-float op. Verified pixel-equivalent to the historical float path
  // (differences only at mathematically exact quantization ties, ~0.2% of AA edge
  // pixels, where the float result was itself rounding-dependent).
  const int64_t dstPixelAreaFP = static_cast<int64_t>(invScaleFP) * invScaleFP;
  int32_t srcY0FP = 0;
  for (int dstY = 0; dstY < dstH; dstY++, srcY0FP += invScaleFP) {
    const int32_t srcY1FP = srcY0FP + invScaleFP;
    const int sy0 = srcY0FP >> FP_SHIFT;
    const int sy1 = std::min(static_cast<int>((srcY1FP - 1) >> FP_SHIFT), srcH - 1);
    int32_t srcX0FP = 0;
    for (int dstX = 0; dstX < dstW; dstX++, srcX0FP += invScaleFP) {
      const int32_t srcX1FP = srcX0FP + invScaleFP;
      const int sx0 = srcX0FP >> FP_SHIFT;
      const int sx1 = std::min(static_cast<int>((srcX1FP - 1) >> FP_SHIFT), srcW - 1);

      int64_t covered = 0;
      for (int sy = sy0; sy <= sy1; sy++) {
        const int32_t hOverlapFP = std::min(srcY1FP, static_cast<int32_t>(sy + 1) << FP_SHIFT) -
                                   std::max(srcY0FP, static_cast<int32_t>(sy) << FP_SHIFT);
        if (hOverlapFP <= 0) continue;
        for (int sx = sx0; sx <= sx1; sx++) {
          const uint8_t raw = srcRaw(sx, sy);
          if (raw == 0) continue;
          const int32_t wOverlapFP = std::min(srcX1FP, static_cast<int32_t>(sx + 1) << FP_SHIFT) -
                                     std::max(srcX0FP, static_cast<int32_t>(sx) << FP_SHIFT);
          if (wOverlapFP <= 0) continue;
          covered += static_cast<int64_t>(raw) * hOverlapFP * wOverlapFP;
        }
      }
      // covered / dstPixelArea is 3x the ink fraction (raw already carries the x3 of the
      // source encoding); round to the nearest level 0..3. Mirrors the source font's raw
      // encoding so the drawMask logic is identical to the body-text per-pixel path.
      const uint8_t raw = static_cast<uint8_t>(std::min<int64_t>(3, (covered + dstPixelAreaFP / 2) / dstPixelAreaFP));
      if ((drawMask >> raw) & 0x01) {
        emit(dstX, dstY);
      }
    }
  }
}
