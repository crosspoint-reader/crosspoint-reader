#pragma once

// Builds a small, deterministic, fixed-width test font for the chapter
// parser tests. Every printable ASCII glyph (plus the bullet U+2022 and the
// replacement glyph U+FFFD) has identical metrics: 10px advance, no kerning,
// no ligatures. This keeps word-wrap/indent arithmetic in the tests simple
// and independent of the real device fonts, while still exercising the real
// EpdFont/EpdFontFamily measurement code (lib/EpdFont), not a reimplementation
// of it.

#include <EpdFontFamily.h>

namespace testfont {

inline EpdFontFamily makeTestFontFamily() {
  static EpdGlyph glyphs[97];
  static EpdUnicodeInterval intervals[3];
  static EpdFontData data{};
  static EpdFont font(&data);
  static bool initialized = false;

  if (!initialized) {
    EpdGlyph g{};
    g.width = 8;
    g.height = 10;
    g.advanceX = static_cast<uint16_t>(10 << 4);  // 10px, 12.4 fixed-point
    g.left = 0;
    g.top = 8;
    g.dataLength = 0;
    g.dataOffset = 0;
    for (auto& slot : glyphs) slot = g;

    // 0x20-0x7E: printable ASCII, 95 contiguous glyphs at offset 0..94.
    intervals[0] = {0x20, 0x7E, 0};
    // U+2022 bullet, used for unordered list markers.
    intervals[1] = {0x2022, 0x2022, 95};
    // U+FFFD replacement glyph, used as the fallback for unmapped codepoints.
    intervals[2] = {0xFFFD, 0xFFFD, 96};

    data.bitmap = nullptr;
    data.glyph = glyphs;
    data.intervals = intervals;
    data.intervalCount = 3;
    data.advanceY = 14;  // line height
    data.ascender = 10;
    data.descender = 2;
    data.is2Bit = false;
    data.groups = nullptr;
    data.groupCount = 0;
    data.glyphToGroup = nullptr;
    data.kernLeftClasses = nullptr;
    data.kernRightClasses = nullptr;
    data.kernMatrix = nullptr;
    data.kernLeftEntryCount = 0;
    data.kernRightEntryCount = 0;
    data.kernLeftClassCount = 0;
    data.kernRightClassCount = 0;
    data.ligaturePairs = nullptr;
    data.ligaturePairCount = 0;
    data.glyphMissHandler = nullptr;
    data.glyphMissCtx = nullptr;

    initialized = true;
  }

  // Bold/italic/boldItalic left null: getFont() falls back to regular for
  // every style, so word metrics don't depend on which style bit is set.
  // The style *bit* itself (as returned by TextBlock::wordStyle()) is what
  // the list-bullet tests assert on, not glyph shape.
  return EpdFontFamily(&font);
}

}  // namespace testfont
