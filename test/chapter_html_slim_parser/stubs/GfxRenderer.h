#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <string>

// Mirror of the real GfxRenderer.h, which owns this enum (upstream design).
namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class GfxRenderer {
 public:
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int, float = 1.0f) const { return 16; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    int width = 0;
    while (*text++) width += 8;
    return width;
  }
  int getTextWidth(int, const char* text, EpdFontFamily::Style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    int width = 0;
    while (*text++) width += 8;
    return width;
  }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 4; }
  bool isSdCardFont(int) const { return false; }
  bool isFontCacheScanning() const { return false; }
  void ensureSdCardFontReady(int, const std::deque<std::string>&, bool, uint8_t) const {}
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style, BidiUtils::BidiBaseDir) const {}
  void drawLine(int, int, int, int, int, bool) const {}
};
