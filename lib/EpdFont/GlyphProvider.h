#pragma once

#include <cstdint>

#include "EpdFontData.h"

/// Supplies what a font's own tables do not hold.
///
/// EpdFont asks it for a glyph after the interval table misses, and for pair kerning when the font
/// carries no kern tables. GfxRenderer asks it whether a glyph's bitmap is one it keeps itself.
/// SdCardFont implements it for its glyph-miss ring; a font with no tables at all can answer every
/// lookup through it.
class GlyphProvider {
 public:
  virtual ~GlyphProvider() = default;

  /// The glyph for `codepoint`, or nullptr when the font has none. The record must stay valid at
  /// least until the next missGlyph() call on ANY provider of the same font: SdCardFont's styles
  /// share one miss ring. Callers consume the record (and its bitmap) before the next lookup.
  virtual const EpdGlyph* missGlyph(uint32_t codepoint) = 0;

  /// True when `glyph` is a record whose bitmap this provider keeps itself. `bitmap` then receives
  /// that bitmap, which is nullptr for a glyph with no pixels. False means the bitmap is at
  /// EpdFontData::bitmap + dataOffset as for any other glyph.
  virtual bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
    (void)glyph;
    (void)bitmap;
    return false;
  }

  /// Pair kerning in 4.4 fixed-point pixels. Asked only when the font carries no kern tables.
  virtual int8_t kerning(uint32_t leftCp, uint32_t rightCp) {
    (void)leftCp;
    (void)rightCp;
    return 0;
  }
};
