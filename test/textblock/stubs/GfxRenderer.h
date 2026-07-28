#pragma once
#include <BidiUtils.h>
#include <EpdFontFamily.h>

#include <string>
#include <vector>

namespace BidiUtils {
// The real GfxRenderer.h declares BidiBaseDir into this namespace (BidiUtils.h
// itself does not), so the stub has to supply it too. Values must match.
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

// Host-test stub for lib/GfxRenderer/GfxRenderer.h. The real renderer owns the
// framebuffer, font cache and e-ink driver. This records the draw calls instead
// of rasterising, and reports deterministic, synthetic font metrics. Only the
// members TextBlock::render() and the serialize paths actually call are here.
//
// Metrics are chosen to make expected positions trivial to compute by hand:
// every codepoint advances GLYPH_W at full size and GLYPH_W/2 at SUP/SUB (the
// real renderer scales those glyphs 50%).
class GfxRenderer {
 public:
  static constexpr int GLYPH_W = 10;
  static constexpr int ASCENDER = 20;
  static constexpr int SCREEN_W = 800;

  struct TextCall {
    int x;
    int y;
    std::string text;
    EpdFontFamily::Style style;
    BidiUtils::BidiBaseDir baseDir;
  };
  struct LineCall {
    int x1;
    int y1;
    int x2;
    int y2;
    int width;
  };

  // Mutable: TextBlock::render() takes the renderer by const reference.
  mutable std::vector<TextCall> textCalls;
  mutable std::vector<LineCall> lineCalls;
  // Recording allocates (a std::string per call, plus vector growth). The
  // allocation-counting tests turn it off so the measured count reflects only
  // what production code allocates.
  mutable bool recording = true;

  // Count UTF-8 codepoints: continuation bytes (0b10xxxxxx) are not advances.
  static int codepoints(const char* text) {
    int n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p) {
      if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
  }

  static bool isHalfScale(const EpdFontFamily::Style style) {
    return (style & EpdFontFamily::SUP) != 0 || (style & EpdFontFamily::SUB) != 0;
  }

  bool isFontCacheScanning() const { return false; }
  int getScreenWidth() const { return SCREEN_W; }
  int getFontAscenderSize(int) const { return ASCENDER; }

  int getTextAdvanceX(int, const char* text, const EpdFontFamily::Style style) const {
    return codepoints(text) * (isHalfScale(style) ? GLYPH_W / 2 : GLYPH_W);
  }

  int getTextWidth(int, const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    return getTextAdvanceX(0, text, style);
  }

  void drawText(int, const int x, const int y, const char* text, bool = true,
                const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                const BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const {
    if (!recording) return;
    textCalls.push_back({x, y, std::string(text), style, baseDir});
  }

  void drawLine(const int x1, const int y1, const int x2, const int y2, const int lineWidth, bool) const {
    if (!recording) return;
    lineCalls.push_back({x1, y1, x2, y2, lineWidth});
  }
};
