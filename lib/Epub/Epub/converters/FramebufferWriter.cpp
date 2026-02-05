#include "FramebufferWriter.h"

void FramebufferWriter::setPixel(int x, int y, bool isBlack) {
  if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t byteIndex = y * DISPLAY_WIDTH_BYTES + (x / 8);
  const uint8_t bitPosition = 7 - (x % 8);

  if (isBlack) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);
  } else {
    frameBuffer[byteIndex] |= (1 << bitPosition);
  }
}

void FramebufferWriter::setPixel2Bit(int x, int y, uint8_t value) {
  if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT || value > 3) {
    return;
  }

  const uint16_t byteIndex = y * DISPLAY_WIDTH_BYTES + (x / 8);
  const uint8_t bitPosition = 7 - (x % 8);

  if (value < 2) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);
  } else {
    frameBuffer[byteIndex] |= (1 << bitPosition);
  }
}