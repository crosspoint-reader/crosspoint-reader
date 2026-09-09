#include "HalDisplay.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Bitmap.h"        // BmpHeader (lib/GfxRenderer)
#include "BitmapHelpers.h"  // createBmpHeader (lib/GfxRenderer)

HalDisplay display;

void HalDisplay::dump() const {
  if (nextOutputPath_.empty()) return;

  // The physical framebuffer is landscape (DISPLAY_WIDTH x DISPLAY_HEIGHT),
  // but GfxRenderer draws in Portrait's logical 480x800 and maps it onto that
  // physical buffer rotated — same reasoning as ScreenshotUtil::
  // saveFramebufferAsBmp's rotation loop (src/util/ScreenshotUtil.cpp),
  // reproduced here so a dumped screen reads right-side-up.
  const int width = DISPLAY_WIDTH;
  const int height = DISPLAY_HEIGHT;
  const int phyWidth = height;
  const int phyHeight = width;
  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;

  BmpHeader header;
  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  FILE* f = std::fopen(nextOutputPath_.c_str(), "wb");
  if (!f) return;
  std::fwrite(&header, sizeof(header), 1, f);

  std::vector<uint8_t> rowBuffer(rowSizePadded);
  for (int outY = 0; outY < phyHeight; outY++) {
    std::fill(rowBuffer.begin(), rowBuffer.end(), 0);
    for (int outX = 0; outX < phyWidth; outX++) {
      const int srcX = width - 1 - outY;
      const int srcY = phyWidth - 1 - outX;
      const int fbIndex = srcY * (width / 8) + (srcX / 8);
      const uint8_t pixel = (buffer_[fbIndex] >> (7 - (srcX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    std::fwrite(rowBuffer.data(), 1, rowSizePadded, f);
  }
  std::fclose(f);
}
