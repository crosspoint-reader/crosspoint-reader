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

  // Fine scale: maps from JPEGDEC-scaled source to final destination
  float fineScale;

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
        fineScale(1.0f),
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

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  int stride = pDraw->iWidth;
  int validW = pDraw->iWidthUsed;
  int blockH = pDraw->iHeight;

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  bool useDithering = ctx->config->useDithering;
  bool caching = ctx->caching;
  float fineScale = ctx->fineScale;
  float invScale = 1.0f / fineScale;

  // Determine destination pixel range covered by this block
  int srcYEnd = pDraw->y + blockH;
  int srcXEnd = pDraw->x + validW;

  int dstYStart = (int)(pDraw->y * fineScale);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)(srcYEnd * fineScale);

  int dstXStart = (int)(pDraw->x * fineScale);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)(srcXEnd * fineScale);

  // Use bilinear interpolation when upscaling (e.g. progressive JPEG DC-only at 1/8).
  // Smooths block boundaries that would otherwise create visible banding.
  bool bilinear = (fineScale > 1.0f);

  for (int dstY = dstYStart; dstY < dstYEnd && dstY < ctx->dstHeight; dstY++) {
    int outY = ctx->config->y + dstY;
    if (outY < 0 || outY >= ctx->screenHeight) continue;

    float srcFy = dstY * invScale;
    int sy0 = (int)srcFy;
    float fy = srcFy - sy0;
    int ly0 = sy0 - pDraw->y;
    int ly1 = ly0 + 1;
    if (ly0 < 0) ly0 = 0;
    if (ly0 >= blockH) ly0 = blockH - 1;
    if (ly1 >= blockH) ly1 = blockH - 1;

    for (int dstX = dstXStart; dstX < dstXEnd && dstX < ctx->dstWidth; dstX++) {
      int outX = ctx->config->x + dstX;
      if (outX < 0 || outX >= ctx->screenWidth) continue;

      float srcFx = dstX * invScale;
      uint8_t gray;

      if (bilinear) {
        // Bilinear interpolation: blend 4 nearest source pixels
        int sx0 = (int)srcFx;
        float fx = srcFx - sx0;
        int lx0 = sx0 - pDraw->x;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        uint8_t p00 = pixels[ly0 * stride + lx0];
        uint8_t p10 = pixels[ly0 * stride + lx1];
        uint8_t p01 = pixels[ly1 * stride + lx0];
        uint8_t p11 = pixels[ly1 * stride + lx1];

        float top = p00 + fx * (p10 - p00);
        float bot = p01 + fx * (p11 - p01);
        gray = (uint8_t)(top + fy * (bot - top));
      } else {
        // Nearest-neighbor for downscale / 1:1
        int col = (int)srcFx - pDraw->x;
        if (col < 0) col = 0;
        if (col >= validW) col = validW - 1;
        gray = pixels[ly0 * stride + col];
      }

      uint8_t dithered;
      if (useDithering) {
        dithered = applyBayerDither4Level(gray, outX, outY);
      } else {
        dithered = gray / 85;
        if (dithered > 3) dithered = 3;
      }
      drawPixelWithRenderMode(*ctx->renderer, outX, outY, dithered);
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
  ctx.fineScale = (float)destWidth / ctx.scaledSrcWidth;

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, ctx.fineScale, isProgressive ? " [progressive]" : "");

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
