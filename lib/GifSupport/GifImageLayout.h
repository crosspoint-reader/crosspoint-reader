#pragma once

#include <algorithm>
#include <cstdint>

namespace GifCommon {
constexpr uint32_t MAX_FILE_BYTES = 16 * 1024 * 1024;
constexpr int MAX_OUTPUT_DIMENSION = 3072;
constexpr uint32_t MAX_BMP_BYTES = 4 * 1024 * 1024;

struct ImageLayout {
  int sourceX = 0;
  int sourceY = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
  int width = 0;
  int height = 0;
};

inline bool calculateLayout(int width, int height, int targetWidth, int targetHeight, bool crop, ImageLayout& out) {
  if (width <= 0 || height <= 0 || width > MAX_OUTPUT_DIMENSION || height > MAX_OUTPUT_DIMENSION ||
      targetWidth <= 0 || targetHeight <= 0 || targetWidth > MAX_OUTPUT_DIMENSION ||
      targetHeight > MAX_OUTPUT_DIMENSION) return false;
  out = {0, 0, width, height, targetWidth, targetHeight};
  if (crop) {
    if (static_cast<int64_t>(width) * targetHeight > static_cast<int64_t>(height) * targetWidth) {
      out.sourceWidth = std::max(1, height * targetWidth / targetHeight);
      out.sourceX = (width - out.sourceWidth) / 2;
    } else {
      out.sourceHeight = std::max(1, width * targetHeight / targetWidth);
      out.sourceY = (height - out.sourceHeight) / 2;
    }
  } else if (static_cast<int64_t>(width) * targetHeight > static_cast<int64_t>(height) * targetWidth) {
    out.height = std::max(1, height * targetWidth / width);
  } else {
    out.width = std::max(1, width * targetHeight / height);
  }
  return true;
}
}  // namespace GifCommon
