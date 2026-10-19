#pragma once
#include <algorithm>

#include "EpdFontData.h"

class EpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY,
                     bool smallCaps = false) const;
  /// Everything getGlyph() does once the interval table has missed: the SD on-demand loader,
  /// then the visual-relative and U+FFFD fallbacks. Out of line on purpose -- it is the rare
  /// path, and inlining the fallback chain into every call site would cost flash for nothing.
  EpdGlyphRef getGlyphSlow(uint32_t cp) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  ~EpdFont() = default;

  /// Interval table plus the provider, with no fallbacks -- so getGlyph()'s fallbacks can never
  /// recurse into one another. Public for FallbackFaceProvider, which serves another font's
  /// missing glyphs and must not answer with this face's U+FFFD or a lookalike.
  EpdGlyphRef findGlyph(uint32_t cp) const;
  void getTextDimensions(const char* string, int* w, int* h, bool smallCaps = false) const;

  /// The glyph for `cp`. When the font has no glyph for it, falls back first to
  /// a close visual relative (see GlyphFallback.h) and then to U+FFFD, so an
  /// uncovered codepoint degrades to something readable rather than a box.
  ///
  /// Returns a resolved value, not a pointer: built-in and SD-card fonts store glyph records in
  /// different shapes. Test it with `if (!glyph)` exactly as before — see EpdGlyphRef.
  ///
  /// The interval search is inline so a caller that reads only, say, advanceX can have the
  /// compiler drop the fields it does not touch; out of line it could not, and measurement paid
  /// for the whole struct on every character. The miss path stays out of line.
  EpdGlyphRef getGlyph(const uint32_t cp) const {
    const int count = data->intervalCount;
    if (count > 0) {
      const EpdUnicodeInterval* intervals = data->intervals;
      // upper_bound: the first interval starting after cp, so the one before it is the only
      // candidate that can contain cp.
      const auto it = std::upper_bound(intervals, intervals + count, cp,
                                       [](uint32_t v, const EpdUnicodeInterval& i) { return v < i.first; });
      if (it != intervals) {
        const EpdUnicodeInterval& interval = *(it - 1);
        if (cp <= interval.last) return epdResolveGlyph(data, interval.offset + (cp - interval.first));
      }
    }
    return getGlyphSlow(cp);
  }

  /// Returns the kerning adjustment (4.4 fixed-point in pixels) between two codepoints.
  /// Returns 0 if no kerning data exists for the pair.
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp) const;

  /// Returns the ligature codepoint for a pair, or 0 if no ligature exists.
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

  /// Greedily applies ligature substitutions starting from cp, consuming
  /// as many following codepoints from text as possible. Returns the
  /// (possibly substituted) codepoint; advances text past consumed chars.
  uint32_t applyLigatures(uint32_t cp, const char*& text) const;
};
