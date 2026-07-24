#pragma once

#include <cstdint>

struct HalDisplay {
  enum RefreshMode { HALF_REFRESH };
};

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  void setOrientation(Orientation) {}
  void displayBuffer() const {}
  void displayBuffer(HalDisplay::RefreshMode) const {}
  bool storeBwBuffer() { return true; }
  void clearScreen(uint8_t = 0xFF) {}
  void setRenderMode(RenderMode) {}
  void copyGrayscaleLsbBuffers() {}
  void copyGrayscaleMsbBuffers() {}
  void displayGrayBuffer() {}
  void restoreBwBuffer() {}
};
