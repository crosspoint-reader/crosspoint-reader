#pragma once

// Minimal host-test stand-in for lib/hal/HalDisplay.h. GfxRenderer.h only
// needs the panel-size constants and the RefreshMode enum to compile (default
// arguments reference HalDisplay::FAST_REFRESH etc.); the test's own
// GfxRenderer.cpp double never calls into a HalDisplay instance, so no
// framebuffer/SPI methods are needed here.

#include <cstdint>

#include "Arduino.h"

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  static constexpr uint16_t DISPLAY_WIDTH = 480;
  static constexpr uint16_t DISPLAY_HEIGHT = 800;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  HalDisplay() = default;
};
