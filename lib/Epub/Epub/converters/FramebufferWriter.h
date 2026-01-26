#pragma once
#include <stdint.h>

class FramebufferWriter {
 private:
  uint8_t* frameBuffer;
  static constexpr int DISPLAY_WIDTH = 800;
  static constexpr int DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;  // 100
  static constexpr int DISPLAY_HEIGHT = 480;

 public:
  explicit FramebufferWriter(uint8_t* framebuffer) : frameBuffer(framebuffer) {}

  // Simple pixel setting for 1-bit rendering
  void setPixel(int x, int y, bool isBlack);

  // 2-bit grayscale pixel setting (for dual-pass rendering)
  void setPixel2Bit(int x, int y, uint8_t value);  // value: 0-3
};