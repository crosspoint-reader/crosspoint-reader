#pragma once
#include "EpdFontData.h"

/// A glyph found by EpdFont. It holds a copy of the metrics rather than a pointer, so a font can
/// store its glyph records in a form other than EpdGlyph.
struct GlyphRef {
  static constexpr uint32_t NO_INDEX = UINT32_MAX;

  EpdGlyph metrics{};
  uint32_t index = NO_INDEX;         ///< Position in the font's glyph table, or NO_INDEX
  const EpdGlyph* loaded = nullptr;  ///< Record returned by glyphMissHandler (SD overflow, TTF)
  bool solid = false;                ///< Synthetic solid block: fill a rectangle, there is no bitmap

  explicit operator bool() const { return index != NO_INDEX || loaded != nullptr || solid; }
  const EpdGlyph* operator->() const { return &metrics; }
};

class EpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  ~EpdFont() = default;
  void getTextDimensions(const char* string, int* w, int* h) const;

  GlyphRef getGlyph(uint32_t cp) const;

  /// Like getGlyph(), but supplies metrics for missing solid block symbols.
  /// If the result is `solid`, draw a filled rectangle; it has no bitmap.
  GlyphRef getGlyphMetrics(uint32_t cp) const;

  /// Returns true if this font covers `cp`: either via its in-RAM interval
  /// table or, for SD card fonts, via the coverageHandler that consults the
  /// full RAM-resident coverage index. Unlike getGlyph(), it never performs
  /// storage I/O and never falls back to the replacement glyph — it reports
  /// only what this font can render. Used by the UI script fallback to
  /// decide whether a string needs to be routed to another font.
  bool hasCodepoint(uint32_t cp) const;

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
