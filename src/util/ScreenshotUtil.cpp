#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    String filename_str = "/screenshots/screenshot-" + String(millis()) + ".bmp";
    if (ScreenshotUtil::saveFramebufferAsBmp(filename_str.c_str(), fb, HalDisplay::DISPLAY_WIDTH,
                                             HalDisplay::DISPLAY_HEIGHT, renderer.getOrientation())) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename_str.c_str());
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, renderer.getScreenWidth() - 12, renderer.getScreenHeight() - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height,
                                          GfxRenderer::Orientation orientation) {
  if (!framebuffer) {
    return false;
  }

  // Determine logical (output BMP) dimensions based on orientation
  int bmpWidth, bmpHeight;
  switch (orientation) {
    case GfxRenderer::Portrait:
    case GfxRenderer::PortraitInverted:
      bmpWidth = height;
      bmpHeight = width;
      break;
    case GfxRenderer::LandscapeClockwise:
    case GfxRenderer::LandscapeCounterClockwise:
    default:
      bmpWidth = width;
      bmpHeight = height;
      break;
  }

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  FsFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, bmpWidth, bmpHeight);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (bmpWidth + 31) / 32 * 4;
  // Derive max row size from the largest display dimension (covers any orientation)
  constexpr int maxDim =
      HalDisplay::DISPLAY_WIDTH > HalDisplay::DISPLAY_HEIGHT ? HalDisplay::DISPLAY_WIDTH : HalDisplay::DISPLAY_HEIGHT;
  constexpr size_t kMaxRowSize = (maxDim + 31) / 32 * 4;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    file.close();
    Storage.remove(filename);
    return false;
  }

  // Transform framebuffer pixels to match the selected orientation on-the-fly
  // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  const int W = width;   // physical panel width (DISPLAY_WIDTH)
  const int H = height;  // physical panel height (DISPLAY_HEIGHT)

  for (int outY = 0; outY < bmpHeight; outY++) {
    for (int outX = 0; outX < bmpWidth; outX++) {
      // Map BMP output pixel to logical coordinates
      // outY=0 is BMP bottom row = logical row (bmpHeight-1)
      int logX = outX;
      int logY = bmpHeight - 1 - outY;

      // Map logical coordinates to physical framebuffer coordinates
      // (same transform as rotateCoordinates in GfxRenderer)
      int phyX, phyY;
      switch (orientation) {
        case GfxRenderer::Portrait:
          phyX = logY;
          phyY = H - 1 - logX;
          break;
        case GfxRenderer::LandscapeClockwise:
          phyX = W - 1 - logX;
          phyY = H - 1 - logY;
          break;
        case GfxRenderer::PortraitInverted:
          phyX = W - 1 - logY;
          phyY = logX;
          break;
        case GfxRenderer::LandscapeCounterClockwise:
        default:
          phyX = logX;
          phyY = logY;
          break;
      }

      int fbIndex = phyY * (W / 8) + (phyX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (phyX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
