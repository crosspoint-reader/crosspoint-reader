#pragma once

#include <EpdFont.h>

#include <cstdint>
#include <memory>

class GfxRenderer;

// A font face drawn at `scale` times the size of a built-in face. Metrics are
// scaled once; each glyph bitmap is scaled the first time it is drawn and then
// kept. Glyphs are resampled from the source's 2-bit coverage by bilinear
// interpolation, so edges stay smooth instead of growing nearest-neighbour
// steps. The face is a regular EpdFontData, so every draw and measure path
// treats it like any other font.
class ScaledFont {
 public:
  ScaledFont() = default;
  ScaledFont(const ScaledFont&) = delete;
  ScaledFont& operator=(const ScaledFont&) = delete;
  ~ScaledFont();

  // `renderer` supplies the source glyph bitmaps (decompressing them when the
  // face is compressed) and must outlive this face.
  bool init(const GfxRenderer& renderer, const EpdFontData& source, float scale);
  const EpdFont* font() const { return font_.get(); }

 private:
  static const uint8_t* bitmapFor(void* ctx, const EpdGlyph* glyph);
  static int8_t kerningFor(void* ctx, uint32_t leftCp, uint32_t rightCp);
  const uint8_t* scaleGlyph(uint32_t index);

  const GfxRenderer* renderer_ = nullptr;
  const EpdFontData* source_ = nullptr;
  std::unique_ptr<EpdFont> sourceFont_;  // for the source's kerning lookup
  int scale256_ = 256;
  uint32_t glyphCount_ = 0;
  std::unique_ptr<EpdGlyph[]> glyphs_;
  std::unique_ptr<uint8_t*[]> bitmaps_;  // per glyph, nullptr until first drawn
  EpdFontData data_{};
  std::unique_ptr<EpdFont> font_;
};
