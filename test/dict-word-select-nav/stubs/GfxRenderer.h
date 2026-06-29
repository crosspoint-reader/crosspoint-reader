#pragma once
// Host-test stub for lib/GfxRenderer/GfxRenderer.h.
// Replaces the full device-bound header with only the surface area that
// WordSelectNavigator.cpp touches.

#include <cstddef>
#include <cstdint>

// Minimal BidiBaseDir definition — referenced by drawText signature.
namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  Orientation getOrientation() const { return orientation_; }
  void setOrientation(Orientation o) { orientation_ = o; }

  // Instrumented rendering methods — counters and call-log are mutable so
  // renderHighlight (which takes const GfxRenderer&) can still be observed.
  void fillRect(int, int, int, int, bool = true) const { ++fillRectCallCount; }

  void drawText(int, int x, int y, const char* text, bool = true, uint8_t = 0,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO) const {
    if (drawCallCount < MAX_DRAW_CALLS) {
      drawCalls[drawCallCount] = {x, y, text};
    }
    ++drawCallCount;
    ++drawTextCallCount;
  }

  // No-op framebuffer methods (only called by HighlightSnapshot).
  size_t readFramebufferRegion(uint16_t, uint16_t, uint16_t, uint16_t, uint8_t*, size_t) const { return 0; }
  void writeFramebufferRegion(uint16_t, uint16_t, uint16_t, uint16_t, const uint8_t*) const {}

  void resetCounters() const {
    fillRectCallCount = 0;
    drawTextCallCount = 0;
    drawCallCount = 0;
  }

  struct DrawCall {
    int x = 0;
    int y = 0;
    const char* text = nullptr;
  };
  static constexpr int MAX_DRAW_CALLS = 8;

  mutable int fillRectCallCount = 0;
  mutable int drawTextCallCount = 0;
  mutable DrawCall drawCalls[MAX_DRAW_CALLS] = {};
  mutable int drawCallCount = 0;

 private:
  Orientation orientation_ = Portrait;
};
