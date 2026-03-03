#include "JpegToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>

#include <cstdlib>
#include <new>

#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
// The file I/O callbacks receive the FsFile* via pFile->fHandle (set by jpegOpen()).
struct JpegContext {
  GfxRenderer* renderer;
  const RenderConfig* config;
  int screenWidth;
  int screenHeight;

  // Source dimensions after JPEGDEC's built-in scaling
  int scaledSrcWidth;
  int scaledSrcHeight;

  // Final output dimensions
  int dstWidth;
  int dstHeight;

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU)
  int32_t fineScaleFP;  // src -> dst mapping
  int32_t invScaleFP;   // dst -> src mapping

  PixelCache cache;
  bool caching;

  JpegContext()
      : renderer(nullptr),
        config(nullptr),
        screenWidth(0),
        screenHeight(0),
        scaledSrcWidth(0),
        scaledSrcHeight(0),
        dstWidth(0),
        dstHeight(0),
        fineScaleFP(1 << 16),
        invScaleFP(1 << 16),
        caching(false) {}
};

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
void* jpegOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("JPG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void jpegClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

// JPEGDEC tracks file position via pFile->iPos internally (e.g. JPEGGetMoreData
// checks iPos < iSize to decide whether more data is available). The callbacks
// MUST maintain iPos to match the actual file position, otherwise progressive
// JPEGs with large headers fail during parsing.
int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t bytesRead = f->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t jpegSeek(JPEGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  if (!f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// JPEGDEC object is ~17 KB due to internal decode buffers.
// Heap-allocate on demand so memory is only used during active decode.
constexpr size_t JPEG_DECODER_APPROX_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = JPEG_DECODER_APPROX_SIZE + 16 * 1024;

// Choose JPEGDEC's built-in scale factor for coarse downscaling.
// Returns the scale denominator (1, 2, 4, or 8) and sets jpegScaleOption.
int chooseJpegScale(float targetScale, int& jpegScaleOption) {
  if (targetScale <= 0.125f) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    return 8;
  }
  if (targetScale <= 0.25f) {
    jpegScaleOption = JPEG_SCALE_QUARTER;
    return 4;
  }
  if (targetScale <= 0.5f) {
    jpegScaleOption = JPEG_SCALE_HALF;
    return 2;
  }
  jpegScaleOption = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const bool useDithering = ctx->config->useDithering;
  const bool caching = ctx->caching;
  const int32_t fineScaleFP = ctx->fineScaleFP;
  const int32_t invScaleFP = ctx->invScaleFP;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFP >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFP >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFP >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFP >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // === 1:1 fast path: no scaling math ===
  if (fineScaleFP == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        drawPixelWithRenderMode(renderer, outX, outY, dithered);
        if (caching) ctx->cache.setPixel(outX, outY, dithered);
      }
    }
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFP > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFP + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFP >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      const int32_t srcFyFP = dstY * invScaleFP;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFP;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        drawPixelWithRenderMode(renderer, outX, outY, dithered);
        if (caching) ctx->cache.setPixel(outX, outY, dithered);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFP;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        drawPixelWithRenderMode(renderer, outX, outY, dithered);
        if (caching) ctx->cache.setPixel(outX, outY, dithered);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFP;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered;
        if (useDithering) {
          dithered = applyBayerDither4Level(gray, outX, outY);
        } else {
          dithered = gray / 85;
          if (dithered > 3) dithered = 3;
        }
        drawPixelWithRenderMode(renderer, outX, outY, dithered);
        if (caching) ctx->cache.setPixel(outX, outY, dithered);
      }
    }
    return 1;
  }

  // === Nearest-neighbor (downscale: fineScale < 1.0) ===
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    const int32_t srcFyFP = dstY * invScaleFP;
    int ly = (srcFyFP >> FP_SHIFT) - blockY;
    if (ly < 0) ly = 0;
    if (ly >= blockH) ly = blockH - 1;
    const uint8_t* row = &pixels[ly * stride];

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFP;
      int lx = (srcFxFP >> FP_SHIFT) - blockX;
      if (lx < 0) lx = 0;
      if (lx >= validW) lx = validW - 1;
      uint8_t gray = row[lx];

      uint8_t dithered;
      if (useDithering) {
        dithered = applyBayerDither4Level(gray, outX, outY);
      } else {
        dithered = gray / 85;
        if (dithered > 3) dithered = 3;
      }
      drawPixelWithRenderMode(renderer, outX, outY, dithered);
      if (caching) ctx->cache.setPixel(outX, outY, dithered);
    }
  }

  return 1;
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_JPEG);
    return false;
  }

  JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder for dimensions");
    return false;
  }

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, nullptr);
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG for dimensions (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    delete jpeg;
    return false;
  }

  out.width = jpeg->getWidth();
  out.height = jpeg->getHeight();
  LOG_DBG("JPG", "Image dimensions: %dx%d", out.width, out.height);

  jpeg->close();
  delete jpeg;
  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_JPEG);
    return false;
  }

  JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = jpeg->open(imagePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "Failed to open JPEG (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    delete jpeg;
    return false;
  }

  int srcWidth = jpeg->getWidth();
  int srcHeight = jpeg->getHeight();

  if (srcWidth <= 0 || srcHeight <= 0) {
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    jpeg->close();
    delete jpeg;
    return false;
  }

  if (!validateImageDimensions(srcWidth, srcHeight, "JPEG")) {
    jpeg->close();
    delete jpeg;
    return false;
  }

  bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (isProgressive) {
    LOG_INF("JPG", "Progressive JPEG detected - decoding DC coefficients only (lower quality)");
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Choose JPEGDEC built-in scaling for coarse downscaling.
  // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only
  // decode produces 1/8 resolution). We must match this to avoid the if/else
  // priority chain in DecodeJPEG selecting a different scale.
  int jpegScaleOption;
  int jpegScaleDenom;
  if (isProgressive) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    jpegScaleDenom = 8;
  } else {
    jpegScaleDenom = chooseJpegScale(targetScale, jpegScaleOption);
  }

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  ctx.fineScaleFP = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFP = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth,
          isProgressive ? " [progressive]" : "");

  // Set pixel type to 8-bit grayscale (must be after open())
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  // Allocate cache buffer using final output dimensions
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.allocate(destWidth, destHeight, config.x, config.y)) {
      LOG_ERR("JPG", "Failed to allocate cache buffer, continuing without caching");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = jpeg->decode(0, 0, jpegScaleOption);
  unsigned long decodeTime = millis() - decodeStart;

  if (rc != 1) {
    LOG_ERR("JPG", "Decode failed (rc=%d, lastError=%d)", rc, jpeg->getLastError());
    jpeg->close();
    delete jpeg;
    return false;
  }

  jpeg->close();
  delete jpeg;
  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms", decodeTime);

  // Write cache file if caching was enabled
  if (ctx.caching) {
    ctx.cache.writeToFile(config.cachePath);
  }

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  std::string ext = extension;
  for (auto& c : ext) {
    c = tolower(c);
  }
  return (ext == ".jpg" || ext == ".jpeg");
}
