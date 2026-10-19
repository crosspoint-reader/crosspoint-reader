#pragma once
// Host-test shim for GfxRenderer — resolves ahead of lib/GfxRenderer/GfxRenderer.h
// via include-path order (this directory is listed before lib/GfxRenderer, which is
// deliberately NOT on the include path of the WordSizeLayoutTest target).
//
// Implements exactly the measurement/draw surface that ParsedText.cpp and
// TextBlock.cpp use, with deterministic fixed-width metrics:
//   - every codepoint advances GLYPH_W pixels (scaled by the per-word scale)
//   - space advance is SPACE_W, kerning is always 0
//   - ascender is ASCENDER, scaled variants multiply and round
//   - font WIDE_FONT (a word font of another family) has WIDE_GLYPH_W, WIDE_SPACE_W and WIDE_ASCENDER instead
// drawText/drawTextScaled calls are recorded so tests can assert baseline
// alignment and per-word scales.

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Epub/CjkPunctuation.h"

class GfxRenderer {
 public:
  static constexpr int GLYPH_W = 10;
  static constexpr int SPACE_W = 5;
  static constexpr int ASCENDER = 16;
  static constexpr int LINE_HEIGHT = 20;
  static constexpr int WIDE_FONT = 7;
  static constexpr int WIDE_GLYPH_W = 15;
  static constexpr int WIDE_SPACE_W = 8;
  static constexpr int WIDE_ASCENDER = 20;
  // A font with no CJK glyphs, as a Latin reader font: it draws each CJK character as its replacement glyph, which
  // advances GLYPH_W like everything else, and hasGlyph says it has none of them.
  static constexpr int LATIN_FONT = 9;
  static int glyphW(const int fontId) { return fontId == WIDE_FONT ? WIDE_GLYPH_W : GLYPH_W; }
  static int ascender(const int fontId) { return fontId == WIDE_FONT ? WIDE_ASCENDER : ASCENDER; }

  struct DrawCall {
    std::string text;
    int fontId;
    int x;
    int y;
    float scale;
    EpdFontFamily::Style style;
  };
  mutable std::vector<DrawCall> drawCalls;

  struct RectCall {
    int x;
    int y;
    int w;
    int h;
    bool state;
  };
  mutable std::vector<RectCall> fillRectCalls;

  static int countCodepoints(const char* text) {
    int count = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
      if ((*p & 0xC0) != 0x80) ++count;  // count UTF-8 lead bytes
    }
    return count;
  }

  // --- Measurement surface used by ParsedText ---
  int getSpaceWidth(const int fontId, EpdFontFamily::Style /*style*/ = EpdFontFamily::REGULAR) const {
    return fontId == WIDE_FONT ? WIDE_SPACE_W : SPACE_W;
  }
  int getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style /*style*/) const {
    return countCodepoints(text) * glyphW(fontId);
  }
  // Where a glyph's ink sits in its advance, as a CJK font draws its marks: an opening bracket on the right half,
  // a closing bracket, 。 and 、 on the left half, the other marks centred; anything else fills its advance.
  bool getGlyphInk(const int fontId, const uint32_t cp, const EpdFontFamily::Style /*style*/, int& inkLeft,
                   int& inkWidth, int& adv) const {
    adv = glyphW(fontId);
    switch (CjkPunctuation::classOf(cp)) {
      case CjkPunctuation::Class::None:
        inkLeft = 0;
        inkWidth = adv;
        break;
      case CjkPunctuation::Class::Opening:
        inkLeft = (adv + 1) / 2;
        inkWidth = adv - inkLeft;
        break;
      case CjkPunctuation::Class::Closing:
      case CjkPunctuation::Class::FullStop:
      case CjkPunctuation::Class::Comma:
        inkLeft = 0;
        inkWidth = adv / 2;
        break;
      default:
        inkLeft = adv / 4;
        inkWidth = adv / 2;
        break;
    }
    return true;
  }
  bool hasGlyph(const int fontId, const uint32_t cp, const EpdFontFamily::Style /*style*/) const {
    return fontId != LATIN_FONT || cp < 0x2E80;
  }
  int getSpaceAdvance(const int fontId, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style style) const {
    return getSpaceWidth(fontId, style);
  }
  int getKerning(int /*fontId*/, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style /*style*/) const {
    return 0;
  }
  int getFontAscenderSize(const int fontId) const { return ascender(fontId); }
  int getFontAscenderSizeScaled(const int fontId, const float scale) const {
    return static_cast<int>(ascender(fontId) * scale + 0.5f);
  }
  int getLineHeight(int /*fontId*/) const { return LINE_HEIGHT; }
  int getLineHeightScaled(int /*fontId*/, const float scale) const {
    return static_cast<int>(LINE_HEIGHT * scale + 0.5f);
  }
  // What layout asked each font to get ready for, in order.
  struct ReadyCall {
    int fontId;
    std::string text;
  };
  mutable std::vector<ReadyCall> readyCalls;
  void ensureFontReady(const int fontId, const char* utf8Text) const { readyCalls.push_back({fontId, utf8Text}); }

  // --- Render surface used by TextBlock ---
  bool isFontCacheScanning() const { return false; }
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, const float scale) const {
    return static_cast<int>(getTextAdvanceX(fontId, text, style) * scale + 0.5f);
  }
  void drawText(int fontId, int x, int y, const char* text, bool /*black*/ = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    drawCalls.push_back({text, fontId, x, y, 1.0f, style});
  }
  void drawTextScaled(int fontId, int x, int y, const char* text, bool /*black*/, EpdFontFamily::Style style,
                      const float scale) const {
    drawCalls.push_back({text, fontId, x, y, scale, style});
  }
  void drawLine(int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, int /*lineWidth*/, bool /*state*/) const {}
  void fillRect(int x, int y, int width, int height, bool state = true) const {
    fillRectCalls.push_back({x, y, width, height, state});
  }
};
