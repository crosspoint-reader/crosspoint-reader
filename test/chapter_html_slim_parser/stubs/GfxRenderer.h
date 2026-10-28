#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer/FontVariantProvider.h>
#include <Utf8.h>

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  mutable size_t fontPreparations = 0;
  std::function<void()> onFontPrepare;
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
  mutable std::string boxPaints;
  void fillRect(int, int, int, int, bool = true) const { boxPaints += 'b'; }
  void fillRectDither(int, int, int, int, Color) const { boxPaints += 's'; }
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
  std::map<int, int> fontLineHeights;
  std::map<int, float> fontSizePixels;
  std::map<int, int> fontAscenders;
  int getLineHeight(int fontId) const {
    const auto found = fontLineHeights.find(fontId);
    return found == fontLineHeights.end() ? 16 : found->second;
  }
  int getLineHeight(int fontId, float compression) const {
    const auto found = fontSizePixels.find(fontId);
    const float size = found == fontSizePixels.end() ? getLineHeight(fontId) : found->second;
    return static_cast<int>(size * compression + 0.5f);
  }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void setFontVariantProvider(const FontVariantProvider& provider) { fontVariantProvider_ = provider; }
  int resolveFontVariant(int fontId, float scale) const { return fontVariantProvider_.resolveFont(fontId, scale); }
  bool ensureFontLoaded(int fontId) const {
    if (fontMap.count(fontId) != 0 || fontId == 0) return true;
    return fontVariantProvider_.load && fontVariantProvider_.load(fontVariantProvider_.ctx, fontId) &&
           fontMap.count(fontId) != 0;
  }
  std::map<int, EpdFontFamily> fontMap;
  int getFontAscenderSize(int fontId) const {
    const auto found = fontAscenders.find(fontId);
    return found == fontAscenders.end() ? 12 : found->second;
  }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  bool sdFont = false;
  mutable size_t coldMeasurements = 0;
  mutable std::set<std::tuple<int, uint8_t, uint32_t>> preparedAdvances;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode = TextMeasureMode::Layout) const {
    int width = 0;
    uint32_t previous = 0;
    while (const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      if (utf8IsCombiningMark(cp)) continue;
      const uint32_t measured = (style & EpdFontFamily::SMALL_CAPS) ? utf8SmallCapsUpper(cp) : cp;
      if (sdFont && !preparedAdvances.count({fontId, static_cast<uint8_t>(style) & 3, measured})) ++coldMeasurements;
      width += 8 + trackingBetween(previous, cp, tracking);
      previous = cp;
    }
    return width;
  }
  int getKerning(int, uint32_t left, uint32_t right, EpdFontFamily::Style, int8_t tracking = 0) const {
    return trackingBetween(left, right, tracking);
  }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return sdFont; }
  void ensureSdCardFontReady(int fontId, const char* text, uint8_t mask, bool includeSmallCaps = false) const {
    const auto* cursor = reinterpret_cast<const uint8_t*>(text);
    while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
      for (uint8_t style = 0; style < 4; ++style) {
        if (mask & (1 << style)) {
          preparedAdvances.emplace(fontId, style, cp);
          if (includeSmallCaps) preparedAdvances.emplace(fontId, style, utf8SmallCapsUpper(cp));
        }
      }
    }
  }
  void ensureSdCardFontReady(int fontId, const char* const* segments, const size_t* lengths, size_t count,
                             bool includeSpace, bool includeHyphen, uint8_t mask, bool includeSmallCaps = false) const {
    ++fontPreparations;
    if (onFontPrepare) onFontPrepare();
    for (size_t i = 0; i < count; ++i) {
      const char* word = segments[i];
      while (word < segments[i] + lengths[i]) {
        ensureSdCardFontReady(fontId, word, mask, includeSmallCaps);
        word += std::char_traits<char>::length(word) + 1;
      }
    }
    if (includeSpace) ensureSdCardFontReady(fontId, " ", mask);
    if (includeHyphen) ensureSdCardFontReady(fontId, "-", mask);
  }

 private:
  FontVariantProvider fontVariantProvider_;
};
