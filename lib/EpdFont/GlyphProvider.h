#pragma once

#include <cstdint>

#include "EpdFontData.h"

class BuildArena;

/// What a provider that renders glyphs needs only for one its font does not hold yet (a TrueType family's scratch
/// block, and in SD mode its page cache), made for the first such lookup rather than held while nothing misses: where
/// that memory may come from, and whether it could be had. A built-in font's group ring (FontDecompressor) is taken
/// from the same lent region, for one group's read.
namespace glyph_miss {

namespace detail {
inline BuildArena* arena = nullptr;
inline uint32_t starved = 0;
inline uint32_t drawn = 0;
}  // namespace detail

/// The region lent to the misses (Scope), or nullptr: they use the heap.
inline BuildArena* arena() { return detail::arena; }

/// Lookups answered with nothing because that memory could not be had, not because the font lacks the glyph: a
/// layout made while the count moved measured such glyphs as missing, and a page drawn while it moved has gaps.
inline uint32_t starvedCount() { return detail::starved; }
inline void noteStarved() { ++detail::starved; }

/// Glyphs drawn that the font did not hold yet: a pass during which this moved paid for their memory.
inline uint32_t drawnCount() { return detail::drawn; }
inline void noteDrawn() { ++detail::drawn; }

/// Lends `lent` to the misses for the scope, nullptr none, and tells whether one went without memory meanwhile. A miss
/// takes a block from the arena and gives it back before its lookup returns, so the owner's blocks stay as they are;
/// the arena must outlive the scope. Nests.
class Scope {
 public:
  explicit Scope(BuildArena* lent) : previous_(detail::arena), starvedBefore_(detail::starved) { detail::arena = lent; }
  ~Scope() { detail::arena = previous_; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  bool starved() const { return detail::starved != starvedBefore_; }

 private:
  BuildArena* previous_ = nullptr;
  uint32_t starvedBefore_ = 0;
};

}  // namespace glyph_miss

/// Supplies what a font's own tables do not hold.
///
/// EpdFont asks it for a glyph after the interval table misses, and for pair kerning when the font
/// carries no kern tables. GfxRenderer asks it whether a glyph's bitmap is one it keeps itself, and
/// lets it prepare a run of text before measuring or drawing it. SdCardFont implements it for its
/// glyph-miss ring; a font with no tables at all (TtfFont) answers every lookup through it.
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

  /// `utf8Text` is about to be measured or drawn in this provider's style: a provider that renders glyphs
  /// can prepare the ones it lacks in one batch. Lookups must still work for anything it skipped.
  virtual void prefetch(const char* utf8Text) { (void)utf8Text; }
};
