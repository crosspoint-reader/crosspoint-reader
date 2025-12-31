#include "ImageBlock.h"

#include <GfxRenderer.h>

void ImageBlock::layout(GfxRenderer& renderer) {
  // ImageBlock doesn't need layout - dimensions are already known
}

void ImageBlock::getScaledDimensions(const int viewportWidth, const int viewportHeight, int* outWidth,
                                     int* outHeight) const {
  if (width <= viewportWidth && height <= viewportHeight) {
    // Image fits, no scaling needed
    *outWidth = width;
    *outHeight = height;
    return;
  }

  // Calculate scale factor to fit within viewport
  float scaleX = static_cast<float>(viewportWidth) / static_cast<float>(width);
  float scaleY = static_cast<float>(viewportHeight) / static_cast<float>(height);
  float scale = (scaleX < scaleY) ? scaleX : scaleY;

  *outWidth = static_cast<int>(static_cast<float>(width) * scale);
  *outHeight = static_cast<int>(static_cast<float>(height) * scale);
}
