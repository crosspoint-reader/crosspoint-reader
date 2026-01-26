#include "PngToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <PNGdec.h>
#include <SDCardManager.h>
#include <SdFat.h>

static FsFile* gPngFile = nullptr;

static void* pngOpenForDims(const char* filename, int32_t* size) { return gPngFile; }

static void pngCloseForDims(void* handle) {}

static int32_t pngReadForDims(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  if (!gPngFile) return 0;
  return gPngFile->read(pBuf, len);
}

static int32_t pngSeekForDims(PNGFILE* pFile, int32_t pos) {
  if (!gPngFile) return -1;
  return gPngFile->seek(pos);
}

// Single static PNG object shared between getDimensions and decode
// (these operations never happen simultaneously)
static PNG png;

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  FsFile file;
  if (!SdMan.openFileForRead("PNG", imagePath, file)) {
    Serial.printf("[%lu] [PNG] Failed to open file for dimensions: %s\n", millis(), imagePath.c_str());
    return false;
  }

  gPngFile = &file;

  int rc = png.open(imagePath.c_str(), pngOpenForDims, pngCloseForDims, pngReadForDims, pngSeekForDims, nullptr);

  if (rc != 0) {
    Serial.printf("[%lu] [PNG] Failed to open PNG for dimensions: %d\n", millis(), rc);
    file.close();
    gPngFile = nullptr;
    return false;
  }

  out.width = png.getWidth();
  out.height = png.getHeight();

  png.close();
  file.close();
  gPngFile = nullptr;
  return true;
}
static GfxRenderer* gRenderer = nullptr;
static const RenderConfig* gConfig = nullptr;
static int gScreenWidth = 0;
static int gScreenHeight = 0;
static FsFile* pngFile = nullptr;

// Scaling state for PNG
static float gScale = 1.0f;
static int gSrcWidth = 0;
static int gSrcHeight = 0;
static int gDstWidth = 0;
static int gDstHeight = 0;
static int gLastDstY = -1;  // Track last rendered destination Y to avoid duplicates

// Pixel cache for PNG (uses scaled dimensions)
static uint8_t* gCacheBuffer = nullptr;
static int gCacheWidth = 0;
static int gCacheHeight = 0;
static int gCacheBytesPerRow = 0;
static int gCacheOriginX = 0;
static int gCacheOriginY = 0;

static void cacheSetPixel(int screenX, int screenY, uint8_t value) {
  if (!gCacheBuffer) return;
  int localX = screenX - gCacheOriginX;
  int localY = screenY - gCacheOriginY;
  if (localX < 0 || localX >= gCacheWidth || localY < 0 || localY >= gCacheHeight) return;

  int byteIdx = localY * gCacheBytesPerRow + localX / 4;
  int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
  gCacheBuffer[byteIdx] = (gCacheBuffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
}

// 4x4 Bayer matrix for ordered dithering
static const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
static uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to ±40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Draw a pixel respecting the current render mode for grayscale support
static void drawPixelWithRenderMode(GfxRenderer* renderer, int x, int y, uint8_t pixelValue) {
  GfxRenderer::RenderMode renderMode = renderer->getRenderMode();
  if (renderMode == GfxRenderer::BW && pixelValue < 3) {
    renderer->drawPixel(x, y, true);
  } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (pixelValue == 1 || pixelValue == 2)) {
    renderer->drawPixel(x, y, false);
  } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && pixelValue == 1) {
    renderer->drawPixel(x, y, false);
  }
}

void* pngOpen(const char* filename, int32_t* size) {
  pngFile = new FsFile();
  if (!SdMan.openFileForRead("PNG", std::string(filename), *pngFile)) {
    delete pngFile;
    pngFile = nullptr;
    return nullptr;
  }
  *size = pngFile->size();
  return pngFile;
}

void pngClose(void* handle) {
  if (pngFile) {
    pngFile->close();
    delete pngFile;
    pngFile = nullptr;
  }
}

int32_t pngRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  if (!pngFile) return 0;
  return pngFile->read(pBuf, len);
}

int32_t pngSeek(PNGFILE* pFile, int32_t pos) {
  if (!pngFile) return -1;
  return pngFile->seek(pos);
}

// Helper to get grayscale from PNG pixel data
static uint8_t getGrayFromPixel(uint8_t* pPixels, int x, int pixelType, uint8_t* palette) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      return pPixels[x];

    case PNG_PIXEL_TRUECOLOR: {
      uint8_t* p = &pPixels[x * 3];
      return (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }

    case PNG_PIXEL_INDEXED: {
      uint8_t paletteIndex = pPixels[x];
      if (palette) {
        uint8_t* p = &palette[paletteIndex * 3];
        return (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      return paletteIndex;
    }

    case PNG_PIXEL_GRAY_ALPHA:
      return pPixels[x * 2];

    case PNG_PIXEL_TRUECOLOR_ALPHA: {
      uint8_t* p = &pPixels[x * 4];
      return (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }

    default:
      return 128;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  if (!gConfig || !gRenderer) return 0;

  int srcY = pDraw->y;
  uint8_t* pPixels = pDraw->pPixels;
  int pixelType = pDraw->iPixelType;

  // Calculate destination Y with scaling
  int dstY = (int)(srcY * gScale);

  // Skip if we already rendered this destination row (multiple source rows map to same dest)
  if (dstY == gLastDstY) return 1;
  gLastDstY = dstY;

  // Check bounds
  if (dstY >= gDstHeight) return 1;

  int outY = gConfig->y + dstY;
  if (outY >= gScreenHeight) return 1;

  // Render scaled row using nearest-neighbor sampling
  for (int dstX = 0; dstX < gDstWidth; dstX++) {
    int outX = gConfig->x + dstX;
    if (outX >= gScreenWidth) continue;

    // Map destination X back to source X
    int srcX = (int)(dstX / gScale);
    if (srcX >= gSrcWidth) srcX = gSrcWidth - 1;

    uint8_t gray = getGrayFromPixel(pPixels, srcX, pixelType, pDraw->pPalette);

    uint8_t ditheredGray;
    if (gConfig->useDithering) {
      ditheredGray = applyBayerDither4Level(gray, outX, outY);
    } else {
      ditheredGray = gray / 85;
      if (ditheredGray > 3) ditheredGray = 3;
    }
    drawPixelWithRenderMode(gRenderer, outX, outY, ditheredGray);
    cacheSetPixel(outX, outY, ditheredGray);
  }

  return 1;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  Serial.printf("[%lu] [PNG] Decoding PNG: %s\n", millis(), imagePath.c_str());

  FsFile file;
  if (!SdMan.openFileForRead("PNG", imagePath, file)) {
    Serial.printf("[%lu] [PNG] Failed to open file: %s\n", millis(), imagePath.c_str());
    return false;
  }

  gRenderer = &renderer;
  gConfig = &config;
  gScreenWidth = renderer.getScreenWidth();
  gScreenHeight = renderer.getScreenHeight();

  int rc = png.open(imagePath.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.printf("[%lu] [PNG] Failed to open PNG: %d\n", millis(), rc);
    file.close();
    gRenderer = nullptr;
    gConfig = nullptr;
    return false;
  }

  if (!validateImageDimensions(png.getWidth(), png.getHeight(), "PNG")) {
    png.close();
    file.close();
    gRenderer = nullptr;
    gConfig = nullptr;
    return false;
  }

  // Calculate scale factor to fit within maxWidth x maxHeight
  gSrcWidth = png.getWidth();
  gSrcHeight = png.getHeight();
  float scaleX = (float)config.maxWidth / gSrcWidth;
  float scaleY = (float)config.maxHeight / gSrcHeight;
  gScale = (scaleX < scaleY) ? scaleX : scaleY;
  if (gScale > 1.0f) gScale = 1.0f;  // Don't upscale

  gDstWidth = (int)(gSrcWidth * gScale);
  gDstHeight = (int)(gSrcHeight * gScale);
  gLastDstY = -1;  // Reset row tracking

  Serial.printf("[%lu] [PNG] PNG %dx%d -> %dx%d (scale %.2f), bpp: %d\n", millis(), gSrcWidth, gSrcHeight, gDstWidth,
                gDstHeight, gScale, png.getBpp());

  if (png.getBpp() != 8) {
    warnUnsupportedFeature("bit depth (" + std::to_string(png.getBpp()) + "bpp)", imagePath);
  }

  if (png.hasAlpha()) {
    warnUnsupportedFeature("alpha channel", imagePath);
  }

  // Allocate cache buffer using SCALED dimensions
  bool caching = !config.cachePath.empty();
  if (caching) {
    gCacheWidth = gDstWidth;
    gCacheHeight = gDstHeight;
    gCacheBytesPerRow = (gCacheWidth + 3) / 4;
    gCacheOriginX = config.x;
    gCacheOriginY = config.y;
    size_t bufferSize = gCacheBytesPerRow * gCacheHeight;
    gCacheBuffer = (uint8_t*)malloc(bufferSize);
    if (gCacheBuffer) {
      memset(gCacheBuffer, 0, bufferSize);
      Serial.printf("[%lu] [PNG] Allocated cache buffer: %d bytes for %dx%d\n", millis(), bufferSize, gCacheWidth,
                    gCacheHeight);
    } else {
      Serial.printf("[%lu] [PNG] Failed to allocate cache buffer, continuing without caching\n", millis());
      caching = false;
    }
  }

  rc = png.decode(nullptr, 0);
  if (rc != PNG_SUCCESS) {
    Serial.printf("[%lu] [PNG] Decode failed: %d\n", millis(), rc);
    png.close();
    file.close();
    gRenderer = nullptr;
    gConfig = nullptr;
    if (gCacheBuffer) {
      free(gCacheBuffer);
      gCacheBuffer = nullptr;
    }
    return false;
  }

  png.close();
  file.close();
  Serial.printf("[%lu] [PNG] PNG decoding complete\n", millis());

  // Write cache file if caching was enabled and buffer was allocated
  if (caching && gCacheBuffer) {
    FsFile cacheFile;
    if (SdMan.openFileForWrite("IMG", config.cachePath, cacheFile)) {
      uint16_t w = gCacheWidth;
      uint16_t h = gCacheHeight;
      cacheFile.write(&w, 2);
      cacheFile.write(&h, 2);
      cacheFile.write(gCacheBuffer, gCacheBytesPerRow * gCacheHeight);
      cacheFile.close();
      Serial.printf("[%lu] [PNG] Cache written: %s (%dx%d, %d bytes)\n", millis(), config.cachePath.c_str(),
                    gCacheWidth, gCacheHeight, 4 + gCacheBytesPerRow * gCacheHeight);
    } else {
      Serial.printf("[%lu] [PNG] Failed to open cache file for writing: %s\n", millis(), config.cachePath.c_str());
    }
    free(gCacheBuffer);
    gCacheBuffer = nullptr;
  }

  gRenderer = nullptr;
  gConfig = nullptr;
  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) const {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".png");
}
