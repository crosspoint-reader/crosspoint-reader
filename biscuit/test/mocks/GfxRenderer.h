#pragma once
// ============================================================
// biscuit. native test mock — GfxRenderer
// No-op renderer that tracks calls for assertions
// ============================================================

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace EpdFontFamily {
  enum Style { REGULAR = 0, BOLD = 1, ITALIC = 2 };
}

namespace HalDisplay {
  enum RefreshMode { FAST_REFRESH = 0, HALF_REFRESH = 1, FULL_REFRESH = 2 };
}

struct Rect {
  int x, y, w, h;
  Rect() : x(0), y(0), w(0), h(0) {}
  Rect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}
};

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void clearScreen(uint8_t = 0xFF) {}
  void displayBuffer(int = 0) {}
  void drawText(int, int, int, const char*, bool = true, int = 0) {}
  void drawCenteredText(int, int, const char*, bool = true, int = 0) {}
  void drawRect(int, int, int, int, bool = true) {}
  void fillRect(int, int, int, int, bool = true) {}
  void drawLine(int, int, int, int) {}
  void drawPixel(int, int, bool = true) {}
  int getTextWidth(int, const char*, int = 0) { return 8 * (int)strlen(const_cast<char*>("")); }
  int getTextHeight(int) { return 20; }
  int getLineHeight(int) { return 24; }
  void invertScreen() {}
  void setOrientation(Orientation) {}
  Orientation getOrientation() const { return Portrait; }
  uint8_t* getFrameBuffer() { return nullptr; }

  std::string truncatedText(int, const char* t, int, int = 0) { return t; }
  std::vector<std::string> wrappedText(int, const char* t, int, int = 10) {
    return {std::string(t)};
  }

  // Track draw calls for test assertions
  struct DrawCall {
    std::string type;
    std::string text;
    int x, y;
  };
  std::vector<DrawCall> drawCalls;
  void clearDrawCalls() { drawCalls.clear(); }
};
