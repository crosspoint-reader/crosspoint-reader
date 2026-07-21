#pragma once

#if defined(CROSSPOINT_SIM_USE_NATIVE_DECODERS)
#if defined(__has_include_next)
#if __has_include_next(<JPEGDEC.h>)
#include_next <JPEGDEC.h>
#else
#error "CROSSPOINT_SIM_USE_NATIVE_DECODERS requires JPEGDEC in this PlatformIO environment"
#endif
#else
#include_next <JPEGDEC.h>
#endif

#else
// Simulator implementation for the JPEGDEC API shape used by firmware.
// It decodes on the host with stb_image and feeds grayscale rows through the
// same draw callback contract as bitbank2/JPEGDEC. This is a preview aid, not
// a hardware-accurate e-ink decode path.

#include "SimulatorImageDecode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Scale options passed to decode()
#define JPEG_SCALE_EIGHTH 4
#define JPEG_SCALE_QUARTER 2
#define JPEG_SCALE_HALF 1

// Pixel output types
#define EIGHT_BIT_GRAYSCALE 1
#define RGB565_LITTLE_ENDIAN 2

// JPEG stream types
#define JPEG_MODE_BASELINE 0
#define JPEG_MODE_PROGRESSIVE 1

struct JPEGFILE {
  void *fHandle;
  int32_t iPos;
  int32_t iSize;
};

struct JPEGDRAW {
  void *pUser;
  uint8_t *pPixels;
  int x;
  int y;
  int iWidth;
  int iHeight;
  int iWidthUsed;
};

using JPEG_DRAW_CALLBACK = int (*)(JPEGDRAW *);
using JPEG_OPEN_CALLBACK = void *(*)(const char *, int32_t *);
using JPEG_CLOSE_CALLBACK = void (*)(void *);
using JPEG_READ_CALLBACK = int32_t (*)(JPEGFILE *, uint8_t *, int32_t);
using JPEG_SEEK_CALLBACK = int32_t (*)(JPEGFILE *, int32_t);

class JPEGDEC {
public:
  int open(const char *filename, JPEG_OPEN_CALLBACK openCb,
           JPEG_CLOSE_CALLBACK closeCb, JPEG_READ_CALLBACK readCb,
           JPEG_SEEK_CALLBACK, JPEG_DRAW_CALLBACK drawCb) {
    image_ = simulator_image::DecodedImage{};
    drawCb_ = drawCb;
    lastError_ = 0;

    if (!filename || !openCb || !closeCb || !readCb) {
      lastError_ = -1;
      return 0;
    }

    int32_t size = 0;
    void *handle = openCb(filename, &size);
    if (!handle || size <= 0) {
      if (handle) {
        closeCb(handle);
      }
      lastError_ = -1;
      return 0;
    }

    std::vector<uint8_t> encoded(static_cast<size_t>(size));
    JPEGFILE file{handle, 0, size};
    int32_t totalRead = 0;
    while (totalRead < size) {
      const int32_t bytesRead =
          readCb(&file, encoded.data() + totalRead, size - totalRead);
      if (bytesRead <= 0) {
        break;
      }
      totalRead += bytesRead;
    }
    closeCb(handle);

    if (totalRead <= 0 ||
        !simulator_image::decodeImageBytes(encoded.data(),
                                           static_cast<size_t>(totalRead), 1,
                                           image_)) {
      lastError_ = -1;
      return 0;
    }

    return 1;
  }
  void close() { image_ = simulator_image::DecodedImage{}; }
  int getWidth() const { return image_.width; }
  int getHeight() const { return image_.height; }
  int getLastError() const { return lastError_; }
  int getJPEGType() const { return JPEG_MODE_BASELINE; }
  void setPixelType(int) {}
  void setUserPointer(void *user) { user_ = user; }
  int decode(int, int, int scaleOption) {
    if (!drawCb_ || image_.pixels.empty() || image_.width <= 0 ||
        image_.height <= 0) {
      lastError_ = -1;
      return 0;
    }

    int scaleDenom = 1;
    if (scaleOption == JPEG_SCALE_EIGHTH) {
      scaleDenom = 8;
    } else if (scaleOption == JPEG_SCALE_QUARTER) {
      scaleDenom = 4;
    } else if (scaleOption == JPEG_SCALE_HALF) {
      scaleDenom = 2;
    }

    const int scaledWidth = (image_.width + scaleDenom - 1) / scaleDenom;
    const int scaledHeight = (image_.height + scaleDenom - 1) / scaleDenom;
    std::vector<uint8_t> row(static_cast<size_t>(scaledWidth));

    for (int y = 0; y < scaledHeight; ++y) {
      const int srcY = std::min(y * scaleDenom, image_.height - 1);
      const uint8_t *srcRow =
          image_.pixels.data() + static_cast<size_t>(srcY) * image_.width;
      for (int x = 0; x < scaledWidth; ++x) {
        const int srcX = std::min(x * scaleDenom, image_.width - 1);
        row[static_cast<size_t>(x)] = srcRow[srcX];
      }

      JPEGDRAW draw{};
      draw.pUser = user_;
      draw.pPixels = row.data();
      draw.x = 0;
      draw.y = y;
      draw.iWidth = scaledWidth;
      draw.iHeight = 1;
      draw.iWidthUsed = scaledWidth;
      if (drawCb_(&draw) == 0) {
        lastError_ = -1;
        return 0;
      }
    }

    return 1;
  }

private:
  simulator_image::DecodedImage image_;
  JPEG_DRAW_CALLBACK drawCb_{nullptr};
  void *user_{nullptr};
  int lastError_{0};
};
#endif
