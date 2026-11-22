#pragma once
#include <cstdint>

#include "Arduino.h"
#include "FontCacheManager.h"

// Only target metadata is simulated; production DirectPixelWriter performs writes.
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  uint8_t* target = nullptr;
  RenderMode mode = BW;
  Orientation orientation = Portrait;
  bool absolute = false;
  bool ditherBw = false;
  int originY = 0;
  int rows = 480;
  int physicalWidth = 800;
  int physicalHeight = 480;
  unsigned placeholders = 0;

  uint8_t* getWriteTarget() const { return target; }
  int getWriteOriginY() const { return originY; }
  int getWriteRows() const { return rows; }
  RenderMode getRenderMode() const { return mode; }
  Orientation getOrientation() const { return orientation; }
  bool grayPlanesAreAbsolute() const { return absolute; }
  bool imagesDitheredInBw() const { return ditherBw; }
  int getDisplayWidthBytes() const { return (physicalWidth + 7) / 8; }
  int getDisplayWidth() const { return physicalWidth; }
  int getDisplayHeight() const { return physicalHeight; }
  int getScreenWidth() const {
    return orientation == Portrait || orientation == PortraitInverted ? physicalHeight : physicalWidth;
  }
  int getScreenHeight() const {
    return orientation == Portrait || orientation == PortraitInverted ? physicalWidth : physicalHeight;
  }
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }
  void preserveImagePolarity(int, int, int, int) {}
  void fillRect(int, int, int, int, bool) { ++placeholders; }
};
