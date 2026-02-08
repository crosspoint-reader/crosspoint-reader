#pragma once

#include <EpdFontFamily.h>

#include <cstring>
#include <map>

#include "Bitmap.h"

// Minimal stub for unit tests — avoids HalDisplay and hardware dependencies
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  GfxRenderer() = default;

  void insertFont(int, EpdFontFamily) {}
  void setOrientation(Orientation) {}
  Orientation getOrientation() const { return Portrait; }

  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }

  // Text measurement — fixed approximations for testing
  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(strlen(text)) * 8;
  }
  int getSpaceWidth(int) const { return 5; }
  int getLineHeight(int) const { return 20; }
  int getTextAdvanceX(int, const char* text) const { return static_cast<int>(strlen(text)) * 8; }
  int getFontAscenderSize(int) const { return 15; }

  // Rendering — no-ops
  void drawText(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawLine(int, int, int, int, bool = true) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawBitmap1Bit(const Bitmap&, int, int, int, int) const {}
};
