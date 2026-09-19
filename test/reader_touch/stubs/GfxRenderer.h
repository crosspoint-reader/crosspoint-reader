#pragma once

#include <cstdint>

struct HalDisplay {
  enum RefreshMode { HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleBase { Separate, Combined };
};

class GfxRenderer {
 public:
  enum class Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  struct GrayscaleCapabilities {
    HalDisplay::GrayscaleBase base = HalDisplay::GrayscaleBase::Separate;
  };

  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void setOrientation(Orientation) {}
  void displayBuffer(HalDisplay::RefreshMode) const {}
  void displayBufferAsync(HalDisplay::RefreshMode) const {}
  void displayGrayscaleBase(HalDisplay::RefreshMode) const {}
  GrayscaleCapabilities grayscaleCapabilities() const { return {}; }
  bool storeBwBuffer() { return true; }
  void cleanupGrayscaleWithFrameBuffer() {}
  void clearScreen(uint8_t) {}
  void setRenderMode(RenderMode) {}
  void copyGrayscaleLsbBuffers() {}
  void copyGrayscaleMsbBuffers() {}
  void displayGrayBuffer() {}
  void restoreBwBuffer() {}
};
