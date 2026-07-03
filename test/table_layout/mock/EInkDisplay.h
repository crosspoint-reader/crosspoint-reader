#pragma once
#include <cstdint>
// Minimal EInkDisplay shim: only the constants HalDisplay.h references.
class EInkDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
};
