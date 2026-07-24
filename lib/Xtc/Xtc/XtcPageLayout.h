#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace xtc {

struct PageLayout {
  size_t rowBytes = 0;
  size_t columnBytes = 0;
  size_t planeBytes = 0;
  size_t payloadBytes = 0;
};

inline bool checkedAdd(const size_t left, const size_t right, size_t& result) {
  if (right > std::numeric_limits<size_t>::max() - left) return false;
  result = left + right;
  return true;
}

inline bool checkedMultiply(const size_t left, const size_t right, size_t& result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) return false;
  result = left * right;
  return true;
}

// XTG is row-major. XTH stores two column-major bit planes, so rounding must
// happen on the height of each column before multiplying by the width.
inline bool calculatePageLayout(const uint16_t width, const uint16_t height, const uint8_t bitDepth,
                                PageLayout& layout) {
  layout = {};
  if (width == 0 || height == 0 || (bitDepth != 1 && bitDepth != 2)) return false;

  if (bitDepth == 1) {
    layout.rowBytes = (static_cast<size_t>(width) + 7U) / 8U;
    return checkedMultiply(layout.rowBytes, height, layout.payloadBytes);
  }

  layout.columnBytes = (static_cast<size_t>(height) + 7U) / 8U;
  if (!checkedMultiply(width, layout.columnBytes, layout.planeBytes)) return false;
  return checkedMultiply(layout.planeBytes, 2U, layout.payloadBytes);
}

inline uint8_t readXthPixel(const uint8_t* payload, const PageLayout& layout, const uint16_t width,
                            const uint16_t x, const uint16_t y) {
  const size_t column = static_cast<size_t>(width - 1U - x);
  const size_t byteOffset = column * layout.columnBytes + y / 8U;
  const uint8_t bit = static_cast<uint8_t>(7U - y % 8U);
  const uint8_t bit0 = static_cast<uint8_t>((payload[byteOffset] >> bit) & 1U);
  const uint8_t bit1 = static_cast<uint8_t>((payload[layout.planeBytes + byteOffset] >> bit) & 1U);
  return static_cast<uint8_t>(bit0 | (bit1 << 1U));
}

struct Viewport {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

inline bool calculateFitViewport(const uint16_t sourceWidth, const uint16_t sourceHeight, const uint16_t screenWidth,
                                 const uint16_t screenHeight, Viewport& viewport) {
  viewport = {};
  if (sourceWidth == 0 || sourceHeight == 0 || screenWidth == 0 || screenHeight == 0) return false;

  uint32_t targetWidth = screenWidth;
  uint32_t targetHeight = screenHeight;
  if (static_cast<uint64_t>(screenWidth) * sourceHeight <=
      static_cast<uint64_t>(screenHeight) * sourceWidth) {
    targetHeight = static_cast<uint32_t>((static_cast<uint64_t>(sourceHeight) * screenWidth) / sourceWidth);
  } else {
    targetWidth = static_cast<uint32_t>((static_cast<uint64_t>(sourceWidth) * screenHeight) / sourceHeight);
  }
  if (targetWidth == 0 || targetHeight == 0) return false;

  viewport.x = static_cast<uint16_t>((screenWidth - targetWidth) / 2U);
  viewport.y = static_cast<uint16_t>((screenHeight - targetHeight) / 2U);
  viewport.width = static_cast<uint16_t>(targetWidth);
  viewport.height = static_cast<uint16_t>(targetHeight);
  return true;
}

inline uint16_t mapViewportCoordinate(const uint16_t destination, const uint16_t destinationSize,
                                      const uint16_t sourceSize) {
  if (destinationSize <= 1 || sourceSize <= 1) return 0;
  return static_cast<uint16_t>((static_cast<uint64_t>(destination) * (sourceSize - 1U)) /
                               (destinationSize - 1U));
}

}  // namespace xtc
