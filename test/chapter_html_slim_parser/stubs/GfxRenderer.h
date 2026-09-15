#pragma once

#include <EpdFontFamily.h>
#include <Utf8.h>

#include <deque>
#include <string>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  class TextSpacingScope {
   public:
    TextSpacingScope(const GfxRenderer& renderer, const int8_t characterSpacing, const uint8_t wordSpacingPercent)
        : renderer_(renderer),
          savedCharacterSpacing_(renderer.characterSpacing_),
          savedWordSpacingPercent_(renderer.wordSpacingPercent_) {
      renderer.characterSpacing_ = characterSpacing;
      renderer.wordSpacingPercent_ = wordSpacingPercent;
    }
    ~TextSpacingScope() {
      renderer_.characterSpacing_ = savedCharacterSpacing_;
      renderer_.wordSpacingPercent_ = savedWordSpacingPercent_;
    }

   private:
    const GfxRenderer& renderer_;
    int8_t savedCharacterSpacing_;
    uint8_t savedWordSpacingPercent_;
  };
  // Fixture metrics: every glyph is 8 px wide, a space is 4 px, kerning is zero.
  mutable int8_t characterSpacing_ = 0;
  mutable uint8_t wordSpacingPercent_ = 100;
  int trackingBetween(const uint32_t left, const uint32_t right) const {
    return left == 0 || left == ' ' || right == ' ' ? 0 : characterSpacing_;
  }
  int scaleSpace(const int px) const { return (px * wordSpacingPercent_ + 50) / 100; }
  bool isFontCacheScanning() const { return false; }
  void drawLine(int, int, int, int, int, bool) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {}
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(font, text, style);
  }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return scaleSpace(4); }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      width += 8 + trackingBetween(previous, cp);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style) const {
    return trackingBetween(left, right);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return scaleSpace(4); }
  bool isSdCardFont(int) const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
};
