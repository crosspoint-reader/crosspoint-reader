#pragma once

#include <EpdFontFamily.h>
#include <Utf8.h>

#include <deque>
#include <map>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum class TextMeasureMode { Layout, Rendered };
  // The fixture has no framebuffer to lend; image probes run as without a loan.
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer&) {}
    void end() {}
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  static int trackingBetween(uint32_t left, uint32_t right, int8_t tracking) {
    return left == 0 || left == ' ' || right == ' ' ? 0 : tracking;
  }
  bool isFontCacheScanning() const { return false; }
  mutable int lastLineY = -1;
  mutable int drawnLineCount = 0;
  void drawLine(int, int y, int, int, int, bool) const {
    lastLineY = y;
    ++drawnLineCount;
  }
  void fillRect(int, int, int, int, bool = true) const {}
  void fillRectDither(int, int, int, int, Color) const {}
  // Fixture glyphs: 8 px advance, capitals 10 px tall.
  int drawScaledCodepoint(int, uint32_t, EpdFontFamily::Style, int, int, int scale256) const {
    return 8 * scale256 / 256;
  }
  bool getCodepointMetrics(int, uint32_t, EpdFontFamily::Style, int32_t& advanceFP, int& top) const {
    advanceFP = 8 << 4;
    top = 10;
    return true;
  }
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {}
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  // Fixture variant provider: maps (fontId, scale) to fontId * 1000 + scale * 100 when set.
  bool variantsEnabled = false;
  int resolveFontVariant(int fontId, float scale) const {
    return variantsEnabled ? fontId * 1000 + static_cast<int>(scale * 100 + 0.5f) : 0;
  }
  bool ensureFontLoaded(int fontId) const { return variantsEnabled || fontMap.count(fontId) != 0 || fontId == 0; }
  std::map<int, EpdFontFamily> fontMap;
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      width += 8 + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const char* const*, const size_t*, size_t, bool, bool, uint8_t) const {}
};
