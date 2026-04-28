#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition

// Static storage for the pending grayscale filename. Lifetime: set by prepareFactoryLutScreenshot,
// consumed (read) inside grayscaleHookCallback when the hook fires. The buffer is never freed —
// it's a fixed-size static so it persists for the lifetime of the firmware.
static char s_pendingFilename[64];

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    String filename_str = "/screenshots/screenshot-" + String(millis()) + ".bmp";
    if (ScreenshotUtil::saveFramebufferAsBmp(filename_str.c_str(), fb, renderer.getDisplayWidth(),
                                             renderer.getDisplayHeight())) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename_str.c_str());
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, renderer.getDisplayHeight() - 12, renderer.getDisplayWidth() - 12, 2, true);
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

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 528px height (X3) after rotation = 68 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 68;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    // Explicitly close() file before calling Storage.remove()
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

  // Explicitly close() file before calling Storage.remove()
  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Grayscale (factory LUT) screenshot — hook-based two-plane capture
// ---------------------------------------------------------------------------

void ScreenshotUtil::prepareFactoryLutScreenshot(GfxRenderer& renderer) {
  snprintf(s_pendingFilename, sizeof(s_pendingFilename), "/screenshots/screenshot-%lu.bmp", millis());
  renderer.setScreenshotHook(grayscaleHookCallback, s_pendingFilename);
}

void ScreenshotUtil::grayscaleHookCallback(const uint8_t* lsbPlane, const uint8_t* msbPlane, int physWidth,
                                           int physHeight, void* ctx) {
  const char* filename = static_cast<const char*>(ctx);
  if (saveGrayscaleBmp(filename, lsbPlane, msbPlane, physWidth, physHeight)) {
    LOG_DBG("SCR", "Grayscale screenshot saved: %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save grayscale screenshot");
  }
}

bool ScreenshotUtil::saveGrayscaleBmp(const char* filename, const uint8_t* lsbPlane, const uint8_t* msbPlane,
                                      int physWidth, int physHeight) {
  // Logical output after 90° CCW rotation (same orientation as BW screenshots):
  //   outWidth  = physHeight  (BMP columns = physical rows)
  //   outHeight = physWidth   (BMP rows    = physical columns)
  const int outWidth = physHeight;
  const int outHeight = physWidth;

  // Ensure /screenshots directory exists.
  if (!Storage.exists("/screenshots")) {
    if (!Storage.mkdir("/screenshots")) {
      return false;
    }
  }

  FsFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to open grayscale screenshot file");
    return false;
  }

  // --- BMP file header (14 bytes) + DIB header (40 bytes) ---
  // 8-bit palette BMP: pixel data offset = 14 + 40 + 256*4 = 1078 bytes.
  const uint32_t rowSizePadded = (static_cast<uint32_t>(outWidth) + 3u) & ~3u;
  const uint32_t imageSize = rowSizePadded * static_cast<uint32_t>(outHeight);
  const uint32_t fileSize = 1078u + imageSize;

#pragma pack(push, 1)
  struct Bmp8Header {
    // File header
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
    // DIB header (BITMAPINFOHEADER)
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
  };
#pragma pack(pop)

  Bmp8Header hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.bfType = 0x4D42u;
  hdr.bfSize = fileSize;
  hdr.bfReserved1 = 0u;
  hdr.bfReserved2 = 0u;
  hdr.bfOffBits = 1078u;
  hdr.biSize = 40u;
  hdr.biWidth = outWidth;
  hdr.biHeight = outHeight;  // positive = bottom-up row order
  hdr.biPlanes = 1u;
  hdr.biBitCount = 8u;
  hdr.biCompression = 0u;  // BI_RGB (uncompressed)
  hdr.biSizeImage = imageSize;
  hdr.biXPelsPerMeter = 2835;
  hdr.biYPelsPerMeter = 2835;
  hdr.biClrUsed = 256u;
  hdr.biClrImportant = 0u;

  bool write_error = false;
  if (file.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr)) != sizeof(hdr)) {
    write_error = true;
  }

  if (!write_error) {
    // Palette: 256 entries × 4 bytes = 1024 bytes.
    // GRAY2_LSB encoding: index = lsb | (msb << 1)
    //   0 → White (0xFF)   lsb=0, msb=0
    //   1 → LightGrey      lsb=1, msb=0
    //   2 → DarkGrey       lsb=0, msb=1
    //   3 → Black (0x00)   lsb=1, msb=1
    static constexpr uint8_t kPalette[16] = {
        0xFF, 0xFF, 0xFF, 0x00,  // index 0: White
        0xAA, 0xAA, 0xAA, 0x00,  // index 1: LightGrey
        0x55, 0x55, 0x55, 0x00,  // index 2: DarkGrey
        0x00, 0x00, 0x00, 0x00,  // index 3: Black
    };
    if (file.write(kPalette, sizeof(kPalette)) != sizeof(kPalette)) {
      write_error = true;
    }
    if (!write_error) {
      // Write 252 zero entries to complete the 256-entry palette table.
      uint8_t zeros[32] = {};
      uint32_t remaining = 252u * 4u;
      while (remaining > 0u && !write_error) {
        const uint32_t chunk = (remaining > sizeof(zeros)) ? static_cast<uint32_t>(sizeof(zeros)) : remaining;
        if (file.write(zeros, chunk) != chunk) write_error = true;
        remaining -= chunk;
      }
    }
  }

  if (write_error) {
    file.close();
    Storage.remove(filename);
    return false;
  }

  // --- Pixel data: one byte per pixel (palette index 0–3), bottom-to-top rows ---
  // Row buffer: outWidth bytes padded to 4-byte boundary (> 256 bytes; heap-allocated).
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(rowSizePadded));
  if (!rowBuf) {
    LOG_ERR("SCR", "saveGrayscaleBmp: malloc failed for row buffer (%u bytes)", rowSizePadded);
    file.close();
    Storage.remove(filename);
    return false;
  }

  // physWidth pixels per physical row; each byte holds 8 pixels (1-bit planes).
  const int bytesPerPhysRow = physWidth / 8;

  for (int outY = 0; outY < outHeight && !write_error; outY++) {
    memset(rowBuf, 0, rowSizePadded);
    for (int outX = 0; outX < outWidth; outX++) {
      // 90° CCW rotation (same transform as BW saveFramebufferAsBmp):
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the logical image.
      const int srcX = physWidth - 1 - outY;
      const int srcY = physHeight - 1 - outX;
      const int byteIdx = srcY * bytesPerPhysRow + (srcX / 8);
      const int bitPos = 7 - (srcX % 8);
      const uint8_t lsb = (lsbPlane[byteIdx] >> bitPos) & 1u;
      const uint8_t msb = (msbPlane[byteIdx] >> bitPos) & 1u;
      rowBuf[outX] = lsb | static_cast<uint8_t>(msb << 1);
    }
    if (file.write(rowBuf, rowSizePadded) != rowSizePadded) write_error = true;
  }

  free(rowBuf);
  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
