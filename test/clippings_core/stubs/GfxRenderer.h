#pragma once

#include <EpdFontFamily.h>

#include <cstring>

class GfxRenderer {
 public:
  GfxRenderer(const int width = 480, const int height = 800, const int lineHeight = 24)
      : width_(width), height_(height), lineHeight_(lineHeight) {}

  int getScreenWidth() const { return width_; }
  int getScreenHeight() const { return height_; }
  int getLineHeight(int) const { return lineHeight_; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    return static_cast<int>(std::strlen(text)) * 6;
  }
  void drawLine(int, int, int, int, int, bool) const {}

 private:
  int width_;
  int height_;
  int lineHeight_;
};
