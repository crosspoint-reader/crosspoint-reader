#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "activities/ActivityManager.h"

void ScreenshotUtil::sanitizeForFat32(const char* input, char* output, size_t maxLen) {
  size_t i = 0;
  for (; input[i] != '\0' && i < maxLen - 1; i++) {
    char c = input[i];
    // Replace FAT32-invalid characters and spaces with dashes
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
        c == ' ') {
      output[i] = '-';
    } else {
      output[i] = c;
    }
  }
  output[i] = '\0';
}

void ScreenshotUtil::buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize) {
  if (info.readerType == ScreenshotInfo::ReaderType::None || info.title[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", millis());
    return;
  }

  char sanitizedTitle[64];
  sanitizeForFat32(info.title, sanitizedTitle, sizeof(sanitizedTitle));

  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
    snprintf(buf, bufSize, "/screenshots/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, info.spineIndex,
             info.currentPage, pct, millis());
  } else {
    snprintf(buf, bufSize, "/screenshots/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, info.currentPage, pct, millis());
  }

  // Truncate title if total path exceeds FAT32 limit
  if (strlen(buf) > 255) {
    size_t titleLen = strlen(sanitizedTitle);
    size_t overhead = strlen(buf) - titleLen;
    if (overhead < 255) {
      size_t maxTitleLen = 255 - overhead;
      sanitizedTitle[maxTitleLen] = '\0';
      if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
        snprintf(buf, bufSize, "/screenshots/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, info.spineIndex,
                 info.currentPage, pct, millis());
      } else {
        snprintf(buf, bufSize, "/screenshots/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, info.currentPage, pct, millis());
      }
    } else {
      snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", millis());
    }
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("SCR", "Framebuffer not available");
    return;
  }

  ScreenshotInfo info = activityManager.getScreenshotInfo();
  char filename[256];
  buildFilename(info, filename, sizeof(filename));

  if (saveFramebufferAsBmp(filename, fb, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT)) {
    LOG_DBG("SCR", "Screenshot saved to %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save screenshot");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, HalDisplay::DISPLAY_HEIGHT - 12, HalDisplay::DISPLAY_WIDTH - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

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

  createBmpHeader(&header, phyWidth, phyHeight);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 480px width = 60 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 64;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    file.close();
    Storage.remove(filename);
    return false;
  }

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    for (int outX = 0; outX < phyWidth; outX++) {
      // 90d counter-clockwise: source (srcX, srcY)
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
      int srcX = width - 1 - outY;     // phyHeight == width
      int srcY = phyWidth - 1 - outX;  // phyWidth == height
      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
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
