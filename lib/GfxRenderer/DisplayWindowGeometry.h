#pragma once

#include <algorithm>
#include <cstdint>

namespace display_window {

struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

template <typename Orientation>
inline void rotateCoordinates(const Orientation orientation, const int x, const int y, int* physicalX, int* physicalY,
                              const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (static_cast<uint8_t>(orientation)) {
    case 0:
      *physicalX = y;
      *physicalY = panelHeight - 1 - x;
      break;
    case 1:
      *physicalX = panelWidth - 1 - x;
      *physicalY = panelHeight - 1 - y;
      break;
    case 2:
      *physicalX = panelWidth - 1 - y;
      *physicalY = x;
      break;
    case 3:
    default:
      *physicalX = x;
      *physicalY = y;
      break;
  }
}

template <typename Orientation>
inline AlignedMemRect screenRectToAlignedMemRect(const Orientation orientation, const int screenX, const int screenY,
                                                 const int screenWidth, const int screenHeight,
                                                 const uint16_t panelWidth, const uint16_t panelHeight) {
  AlignedMemRect result;
  if (screenWidth <= 0 || screenHeight <= 0) return result;

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  rotateCoordinates(orientation, screenX, screenY, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, screenX + screenWidth - 1, screenY + screenHeight - 1, &x1, &y1, panelWidth,
                    panelHeight);

  const int memoryXLow = std::min(x0, x1);
  const int memoryYLow = std::min(y0, y1);
  const int memoryXHigh = std::max(x0, x1) + 1;
  const int memoryYHigh = std::max(y0, y1) + 1;
  const int alignedXLow = std::max(0, memoryXLow & ~0x7);
  const int alignedXHigh = std::min<int>(panelWidth, (memoryXHigh + 7) & ~0x7);
  const int clampedYLow = std::max(0, memoryYLow);
  const int clampedYHigh = std::min<int>(panelHeight, memoryYHigh);
  if (alignedXHigh <= alignedXLow || clampedYHigh <= clampedYLow) return result;

  result.x = static_cast<uint16_t>(alignedXLow);
  result.y = static_cast<uint16_t>(clampedYLow);
  result.w = static_cast<uint16_t>(alignedXHigh - alignedXLow);
  result.h = static_cast<uint16_t>(clampedYHigh - clampedYLow);
  result.valid = true;
  return result;
}

}  // namespace display_window
