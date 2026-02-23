#include "PngToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PNGdec.h>

#include <cstdlib>
#include <new>

#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the FsFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer;
  const RenderConfig* config;
  int screenWidth;
  int screenHeight;

  // Scaling state
  float scale;
  int srcWidth;
  int srcHeight;
  int dstWidth;
  int dstHeight;
  int lastDstY;  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching;

  uint8_t* grayLineBuffer;

  PngContext()
      : renderer(nullptr),
        config(nullptr),
        screenWidth(0),
        screenHeight(0),
        scale(1.0f),
        srcWidth(0),
        srcHeight(0),
        dstWidth(0),
        dstHeight(0),
        lastDstY(-1),
        caching(false),
        grayLineBuffer(nullptr) {}
};

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is ~42 KB due to internal zlib decompression buffers.
// We heap-allocate it on demand rather than using a static instance, so this memory
// is only consumed while actually decoding/querying PNG images. This is critical on
// the ESP32-C3 where total RAM is ~320 KB.
constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024;                          // ~42 KB + overhead
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 16 * 1024;  // decoder + 16 KB headroom

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int requiredPngInternalBufferBytes(int srcWidth, int pixelType) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  return ((pitch + 1) * 2) + 32;
}

// Convert entire source line to grayscale with alpha blending to white background.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      memcpy(grayLine, pPixels, width);
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = pPixels[x];
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t* p = &palette[pPixels[x] * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        memcpy(grayLine, pPixels, width);
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Calculate destination Y with scaling
  int dstY = (int)(srcY * ctx->scale);

  // Skip if we already rendered this destination row (multiple source rows map to same dest)
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;

  // Check bounds
  if (dstY >= ctx->dstHeight) return 1;

  int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return 1;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->pPalette,
                    pDraw->iHasAlpha);

  // Render scaled row using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;

  int srcX = 0;
  int error = 0;

  for (int dstX = 0; dstX < dstWidth; dstX++) {
    int outX = outXBase + dstX;
    if (outX < screenWidth) {
      uint8_t gray = ctx->grayLineBuffer[srcX];

      uint8_t ditheredGray;
      if (useDithering) {
        ditheredGray = applyBayerDither4Level(gray, outX, outY);
      } else {
        ditheredGray = gray / 85;
        if (ditheredGray > 3) ditheredGray = 3;
      }
      drawPixelWithRenderMode(*ctx->renderer, outX, outY, ditheredGray);
      if (caching) ctx->cache.setPixel(outX, outY, ditheredGray);
    }

    // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder for dimensions");
    return false;
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);

  if (rc != 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %d", rc);
    delete png;
    return false;
  }

  out.width = png->getWidth();
  out.height = png->getHeight();

  png->close();
  delete png;
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function
  PNG* png = new (std::nothrow) PNG();
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    delete png;
    return false;
  }

  if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {
    png->close();
    delete png;
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.scale, png->getBpp());

  const int pixelType = png->getPixelType();
  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("PNG",
            "PNG row buffer too small: need %d bytes for width=%d type=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
            requiredInternal, ctx.srcWidth, pixelType, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    png->close();
    delete png;
    return false;
  }

  if (png->getBpp() != 8) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png->getBpp()) + "bpp)", imagePath);
  }

  // Allocate grayscale line buffer on demand (~3.2 KB) - freed after decode
  const size_t grayBufSize = PNG_MAX_BUFFERED_PIXELS / 2;
  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  if (!ctx.grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    png->close();
    delete png;
    return false;
  }

  // Allocate cache buffer using SCALED dimensions
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.allocate(ctx.dstWidth, ctx.dstHeight, config.x, config.y)) {
      LOG_ERR("PNG", "Failed to allocate cache buffer, continuing without caching");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  free(ctx.grayLineBuffer);
  ctx.grayLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    png->close();
    delete png;
    return false;
  }

  png->close();
  delete png;
  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Write cache file if caching was enabled and buffer was allocated
  if (ctx.caching) {
    ctx.cache.writeToFile(config.cachePath);
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".png");
}
