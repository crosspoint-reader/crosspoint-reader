#include "PngToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <PNGdec.h>
#include <SDCardManager.h>
#include <SdFat.h>

#include "DitherUtils.h"
#include "PixelCache.h"

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
        caching(false) {}
};

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
static void* pngOpenWithHandle(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!SdMan.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

static void pngCloseWithHandle(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

static int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

static int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Single static PNG object shared between getDimensions and decode
// (these operations never happen simultaneously)
static PNG png;

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  int rc =
      png.open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle, nullptr);

  if (rc != 0) {
    Serial.printf("[%lu] [PNG] Failed to open PNG for dimensions: %d\n", millis(), rc);
    return false;
  }

  out.width = png.getWidth();
  out.height = png.getHeight();

  png.close();
  return true;
}

// Convert entire source line to grayscale with alpha blending to white background.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
static void convertLineToGray(uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, uint8_t* palette,
                              int hasAlpha) {
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

// Stack buffer for grayscale line conversion (max width from PNGdec)
static uint8_t grayLineBuffer[PNG_MAX_BUFFERED_PIXELS / 2];

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

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
  convertLineToGray(pDraw->pPixels, grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->pPalette, pDraw->iHasAlpha);

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
      uint8_t gray = grayLineBuffer[srcX];

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

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  Serial.printf("[%lu] [PNG] Decoding PNG: %s\n", millis(), imagePath.c_str());

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png.open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                    pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.printf("[%lu] [PNG] Failed to open PNG: %d\n", millis(), rc);
    return false;
  }

  if (!validateImageDimensions(png.getWidth(), png.getHeight(), "PNG")) {
    png.close();
    return false;
  }

  // Calculate scale factor to fit within maxWidth x maxHeight
  ctx.srcWidth = png.getWidth();
  ctx.srcHeight = png.getHeight();
  float scaleX = (float)config.maxWidth / ctx.srcWidth;
  float scaleY = (float)config.maxHeight / ctx.srcHeight;
  ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
  if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

  ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
  ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  ctx.lastDstY = -1;  // Reset row tracking

  Serial.printf("[%lu] [PNG] PNG %dx%d -> %dx%d (scale %.2f), bpp: %d\n", millis(), ctx.srcWidth, ctx.srcHeight,
                ctx.dstWidth, ctx.dstHeight, ctx.scale, png.getBpp());

  if (png.getBpp() != 8) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png.getBpp()) + "bpp)", imagePath);
  }

  // Allocate cache buffer using SCALED dimensions
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.allocate(ctx.dstWidth, ctx.dstHeight, config.x, config.y)) {
      Serial.printf("[%lu] [PNG] Failed to allocate cache buffer, continuing without caching\n", millis());
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = png.decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;
  if (rc != PNG_SUCCESS) {
    Serial.printf("[%lu] [PNG] Decode failed: %d\n", millis(), rc);
    png.close();
    return false;
  }

  png.close();
  Serial.printf("[%lu] [PNG] PNG decoding complete - render time: %lu ms\n", millis(), decodeTime);

  // Write cache file if caching was enabled and buffer was allocated
  if (ctx.caching) {
    ctx.cache.writeToFile(config.cachePath);
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) const {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".png");
}
