#pragma once

#if defined(CROSSPOINT_SIM_USE_NATIVE_DECODERS)
#if defined(__has_include_next)
#if __has_include_next(<PNGdec.h>)
#include_next <PNGdec.h>
#else
#error "CROSSPOINT_SIM_USE_NATIVE_DECODERS requires PNGdec in this PlatformIO environment"
#endif
#else
#include_next <PNGdec.h>
#endif

#else
// Simulator implementation for the PNGdec API shape used by firmware.
// It decodes on the host with stb_image and feeds RGBA rows through the same
// draw callback contract as bitbank2/PNGdec. This is a preview aid, not a
// hardware-accurate e-ink decode path.

#include "SimulatorImageDecode.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#define PNG_SUCCESS 0
#define PNG_INVALID_FILE -1

// Pixel types
#define PNG_PIXEL_GRAYSCALE 0
#define PNG_PIXEL_TRUECOLOR 2
#define PNG_PIXEL_INDEXED 3
#define PNG_PIXEL_GRAY_ALPHA 4
#define PNG_PIXEL_TRUECOLOR_ALPHA 6

struct PNGFILE {
  void *fHandle;
};

struct PNGDRAW {
  void *pUser;
  uint8_t *pPixels;
  uint8_t *pPalette;
  int y;
  int iPixelType;
  int iBpp;
  int iHasAlpha;
  int iWidth;
};

using PNG_DRAW_CALLBACK = int (*)(PNGDRAW *);
using PNG_OPEN_CALLBACK = void *(*)(const char *, int32_t *);
using PNG_CLOSE_CALLBACK = void (*)(void *);
using PNG_READ_CALLBACK = int32_t (*)(PNGFILE *, uint8_t *, int32_t);
using PNG_SEEK_CALLBACK = int32_t (*)(PNGFILE *, int32_t);

class PNG {
public:
  int open(const char *filename, PNG_OPEN_CALLBACK openCb,
           PNG_CLOSE_CALLBACK closeCb, PNG_READ_CALLBACK readCb,
           PNG_SEEK_CALLBACK, PNG_DRAW_CALLBACK drawCb) {
    image_ = simulator_image::DecodedImage{};
    drawCb_ = drawCb;

    if (!filename || !openCb || !closeCb || !readCb) {
      return PNG_INVALID_FILE;
    }

    int32_t size = 0;
    void *handle = openCb(filename, &size);
    if (!handle || size <= 0) {
      if (handle) {
        closeCb(handle);
      }
      return PNG_INVALID_FILE;
    }

    std::vector<uint8_t> encoded(static_cast<size_t>(size));
    PNGFILE file{handle};
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
                                           static_cast<size_t>(totalRead), 4,
                                           image_)) {
      return PNG_INVALID_FILE;
    }

    return PNG_SUCCESS;
  }
  void close() { image_ = simulator_image::DecodedImage{}; }
  int getWidth() const { return image_.width; }
  int getHeight() const { return image_.height; }
  int getBpp() const { return 8; }
  int getPixelType() const { return PNG_PIXEL_TRUECOLOR_ALPHA; }
  int hasAlpha() const { return image_.hasAlpha ? 1 : 0; }
  uint32_t getTransparentColor() const { return 0; }
  int decode(void *user, int) {
    if (!drawCb_ || image_.pixels.empty() || image_.width <= 0 ||
        image_.height <= 0) {
      return PNG_INVALID_FILE;
    }

    const size_t rowStride = static_cast<size_t>(image_.width) * 4;
    for (int y = 0; y < image_.height; ++y) {
      PNGDRAW draw{};
      draw.pUser = user;
      draw.pPixels = image_.pixels.data() + static_cast<size_t>(y) * rowStride;
      draw.pPalette = nullptr;
      draw.y = y;
      draw.iPixelType = PNG_PIXEL_TRUECOLOR_ALPHA;
      draw.iBpp = getBpp();
      draw.iHasAlpha = image_.hasAlpha ? 1 : 0;
      draw.iWidth = image_.width;
      if (drawCb_(&draw) == 0) {
        return PNG_INVALID_FILE;
      }
    }

    return PNG_SUCCESS;
  }

private:
  simulator_image::DecodedImage image_;
  PNG_DRAW_CALLBACK drawCb_{nullptr};
};
#endif
