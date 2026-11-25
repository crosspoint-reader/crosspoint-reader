#include "ScaledFont.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "GfxRenderer.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace {

// Glyph caches go to PSRAM when the board has it.
uint8_t* allocZeroed(const size_t bytes) {
#if defined(ESP_PLATFORM)
  void* p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return static_cast<uint8_t*>(p);
#endif
  return static_cast<uint8_t*>(calloc(1, bytes));
}

int scaleRounded(const int v, const int scale256) { return (v * scale256 + (v >= 0 ? 128 : -128)) / 256; }

int scaleUp(const int v, const int scale256) { return (v * scale256 + 255) / 256; }

}  // namespace

ScaledFont::~ScaledFont() {
  if (!bitmaps_) return;
  for (uint32_t i = 0; i < glyphCount_; ++i) free(bitmaps_[i]);
}

bool ScaledFont::init(const GfxRenderer& renderer, const EpdFontData& source, const float scale) {
  // Built-in faces only: SD and vector fonts already fetch glyphs through these hooks.
  if (source.glyphMissHandler || source.vectorBitmapHandler || source.kernHandler || scale <= 0.0f) return false;
  renderer_ = &renderer;
  source_ = &source;
  sourceFont_ = std::make_unique<EpdFont>(&source);
  scale256_ = static_cast<int>(scale * 256.0f + 0.5f);

  for (uint32_t i = 0; i < source.intervalCount; ++i) {
    const EpdUnicodeInterval& interval = source.intervals[i];
    glyphCount_ = std::max(glyphCount_, interval.offset + (interval.last - interval.first) + 1);
  }
  glyphs_ = std::make_unique<EpdGlyph[]>(glyphCount_);
  bitmaps_ = std::make_unique<uint8_t*[]>(glyphCount_);
  for (uint32_t i = 0; i < glyphCount_; ++i) {
    const EpdGlyph& src = source.glyph[i];
    EpdGlyph& dst = glyphs_[i];
    dst.width = static_cast<uint8_t>(std::min(255, scaleUp(src.width, scale256_)));
    dst.height = static_cast<uint8_t>(std::min(255, scaleUp(src.height, scale256_)));
    dst.advanceX = static_cast<uint16_t>(std::min(0xFFFF, scaleRounded(src.advanceX, scale256_)));
    dst.left = static_cast<int16_t>(scaleRounded(src.left, scale256_));
    dst.top = static_cast<int16_t>(scaleRounded(src.top, scale256_));
    dst.dataLength = 0;
    dst.dataOffset = 0;
    bitmaps_[i] = nullptr;
  }

  // Same intervals and ligatures; scaled metrics; bitmaps and kerning through
  // the handlers; no compressed groups or kerning tables of its own.
  data_ = source;
  data_.bitmap = nullptr;
  data_.glyph = glyphs_.get();
  data_.advanceY = static_cast<uint8_t>(std::min(255, scaleRounded(source.advanceY, scale256_)));
  data_.ascender = scaleRounded(source.ascender, scale256_);
  data_.descender = scaleRounded(source.descender, scale256_);
  data_.is2Bit = true;
  data_.groups = nullptr;
  data_.groupCount = 0;
  data_.glyphToGroup = nullptr;
  data_.glyphMissCtx = this;
  data_.vectorBitmapHandler = &ScaledFont::bitmapFor;
  data_.kernHandler = &ScaledFont::kerningFor;
  font_ = std::make_unique<EpdFont>(&data_);
  return true;
}

const uint8_t* ScaledFont::bitmapFor(void* ctx, const EpdGlyph* glyph) {
  auto* self = static_cast<ScaledFont*>(ctx);
  const auto index = static_cast<uint32_t>(glyph - self->glyphs_.get());
  if (index >= self->glyphCount_) return nullptr;
  if (!self->bitmaps_[index]) self->bitmaps_[index] = const_cast<uint8_t*>(self->scaleGlyph(index));
  return self->bitmaps_[index];
}

int8_t ScaledFont::kerningFor(void* ctx, const uint32_t leftCp, const uint32_t rightCp) {
  auto* self = static_cast<ScaledFont*>(ctx);
  const int kern = scaleRounded(self->sourceFont_->getKerning(leftCp, rightCp), self->scale256_);
  return static_cast<int8_t>(std::clamp(kern, -128, 127));
}

const uint8_t* ScaledFont::scaleGlyph(const uint32_t index) {
  const EpdGlyph& src = source_->glyph[index];
  const EpdGlyph& dst = glyphs_[index];
  if (dst.width == 0 || dst.height == 0) return nullptr;
  const uint8_t* bitmap = renderer_->getGlyphBitmap(source_, &src);
  if (!bitmap) return nullptr;

  const int srcW = src.width;
  const int srcH = src.height;
  const bool is2Bit = source_->is2Bit;
  // Coverage 0 (white) .. 3 (black); 1-bit sources read 0 or 3; 0 outside the bitmap.
  const auto sample = [&](const int sx, const int sy) -> int {
    if (sx < 0 || sy < 0 || sx >= srcW || sy >= srcH) return 0;
    const int pos = sy * srcW + sx;
    if (is2Bit) return (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
    return ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

  const int dstW = dst.width;
  const int dstH = dst.height;
  // Source position of each destination pixel centre, 16.16 fixed point,
  // shifted by (ox, oy) in 1/256 source pixels.
  const int32_t step = (256 << 16) / scale256_;
  const int32_t origin = step / 2 - (1 << 15);
  constexpr int FULL = 3 << 16;  // interpolated coverage of solid black
  const auto coverageAt = [&](const int dx, const int dy, const int ox, const int oy) {
    const int32_t sx = origin + dx * step + (ox << 8);
    const int32_t sy = origin + dy * step + (oy << 8);
    const int x0 = sx >> 16, y0 = sy >> 16, fx = (sx >> 8) & 0xFF, fy = (sy >> 8) & 0xFF;
    const int upper = sample(x0, y0) * (256 - fx) + sample(x0 + 1, y0) * fx;
    const int lower = sample(x0, y0 + 1) * (256 - fx) + sample(x0 + 1, y0 + 1) * fx;
    return upper * (256 - fy) + lower * fy;
  };

  // Grid fit: at a fractional scale each source column lands at a different
  // pixel phase, so equal stems round to unequal widths. Shift the sampling
  // grid by up to 3/8 of a source pixel, horizontally then vertically, to the
  // offset whose pixels are least ambiguous (closest to white or black).
  const auto ambiguity = [&](const int ox, const int oy) {
    int64_t total = 0;
    for (int dy = 0; dy < dstH; ++dy) {
      for (int dx = 0; dx < dstW; ++dx) {
        const int c = coverageAt(dx, dy, ox, oy);
        total += std::min(c, FULL - c);
      }
    }
    return total;
  };
  const auto bestOffset = [&](const auto& ambiguityFor) {
    int best = 0;
    int64_t bestAmbiguity = ambiguityFor(0);
    for (int offset = -96; offset <= 96; offset += 32) {
      const int64_t a = offset == 0 ? bestAmbiguity : ambiguityFor(offset);
      if (a < bestAmbiguity) {
        bestAmbiguity = a;
        best = offset;
      }
    }
    return best;
  };
  const int ox = bestOffset([&](const int o) { return ambiguity(o, 0); });
  const int oy = bestOffset([&](const int o) { return ambiguity(ox, o); });

  uint8_t* out = allocZeroed((static_cast<size_t>(dstW) * dstH + 3) / 4);
  if (!out) return nullptr;
  // The B/W pass paints every non-white level, so INK_FROM sets the stroke
  // weight: with the grid fit, 30% coverage matched the stem widths of
  // natively rasterized Ubuntu at 1.6x from 2-bit sources. The three ink
  // levels split the rest of the range.
  constexpr int INK_FROM = FULL * 3 / 10;
  for (int dy = 0; dy < dstH; ++dy) {
    for (int dx = 0; dx < dstW; ++dx) {
      const int coverage = coverageAt(dx, dy, ox, oy);
      const int level = coverage < INK_FROM ? 0 : std::min(3, 1 + 3 * (coverage - INK_FROM) / (FULL - INK_FROM));
      const int pos = dy * dstW + dx;
      out[pos >> 2] |= static_cast<uint8_t>(level << ((3 - (pos & 3)) * 2));
    }
  }
  return out;
}
