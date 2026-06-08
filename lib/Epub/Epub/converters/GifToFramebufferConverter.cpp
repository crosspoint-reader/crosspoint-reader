#include "GifToFramebufferConverter.h"

#include <AnimatedGIF.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include <GifCommon.h>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

struct GifContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  float scale{1.f};
  int lastDstY{-1};
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  PixelCache cache;
  bool caching{false};
  bool success{true};
};

void updateGrayPalette(const GIFDRAW* pDraw, GifContext& ctx) {
  const uint8_t* palette24 = pDraw->pPalette24;
  if (palette24 == nullptr || palette24 == ctx.activePalette24) return;

  ctx.activePalette24 = palette24;
  for (int i = 0; i < 256; ++i) {
    const uint8_t* pixel = &palette24[i * 3];
    ctx.grayPalette[i] = static_cast<uint8_t>((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
  }
}

void renderCanvasRow(const uint8_t* grayRow, int srcY, GifContext& ctx) {
  const int dstY = static_cast<int>(srcY * ctx.scale);
  if (dstY == ctx.lastDstY) return;
  ctx.lastDstY = dstY;
  if (dstY >= ctx.dstHeight) return;

  const int outY = ctx.config->y + dstY;
  if (outY >= ctx.screenHeight) return;

  DirectPixelWriter pw;
  pw.init(*ctx.renderer);
  pw.beginRow(outY);

  bool caching = ctx.caching;
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx.cache.advanceTo(dstY)) {
      caching = false;
      ctx.caching = false;
    } else {
      cw.init(ctx.cache.buffer, ctx.cache.bytesPerRow, ctx.cache.bandRows, ctx.cache.originX);
      cw.beginRow(outY, ctx.config->y + ctx.cache.bandStart);
    }
  }

  int srcX = 0;
  int error = 0;
  const int outXBase = ctx.config->x;
  const bool useDithering = ctx.config->useDithering;

  for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
    const int outX = outXBase + dstX;
    if (outX < ctx.screenWidth) {
      const uint8_t gray = grayRow[srcX];
      uint8_t ditheredGray;
      if (useDithering) {
        ditheredGray = applyBayerDither4Level(gray, outX, outY);
      } else {
        ditheredGray = gray / 85;
        if (ditheredGray > 3) ditheredGray = 3;
      }
      pw.writePixel(outX, ditheredGray);
      if (caching) cw.writePixel(outX, ditheredGray);
    }

    error += ctx.srcWidth;
    while (error >= ctx.dstWidth) {
      error -= ctx.dstWidth;
      srcX++;
      if (srcX >= ctx.srcWidth) {
        srcX = ctx.srcWidth - 1;
        break;
      }
    }
  }
}

void processGapRowsUpTo(int canvasY, GifContext& ctx) {
  while (ctx.success && ctx.nextExpectedSrcY < canvasY && ctx.nextExpectedSrcY < ctx.srcHeight) {
    renderCanvasRow(ctx.whiteRow.get(), ctx.nextExpectedSrcY, ctx);
    ctx.nextExpectedSrcY++;
  }
}

void* gifOpenWithHandle(const char* filename, int32_t* size) {
  auto file = makeUniqueNoThrow<HalFile>();
  if (!file) return nullptr;
  if (!Storage.openFileForRead("GIF", std::string(filename), *file)) {
    return nullptr;
  }
  *size = static_cast<int32_t>(file->size());
  return file.release();
}

void gifCloseWithHandle(void* handle) {
  auto* file = reinterpret_cast<HalFile*>(handle);
  if (!file) return;
  file->close();
  delete file;
}

int32_t gifReadWithHandle(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* file = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!file) return 0;
  int32_t bytesRead = file->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t gifSeekWithHandle(GIFFILE* pFile, int32_t position) {
  auto* file = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!file) return -1;
  if (position < 0) position = 0;
  if (position >= pFile->iSize) position = pFile->iSize - 1;
  if (!file->seek(position)) return -1;
  pFile->iPos = position;
  return position;
}

void gifDrawCallback(GIFDRAW* pDraw) {
  auto* ctx = reinterpret_cast<GifContext*>(pDraw->pUser);
  if (!ctx || !ctx->success || !ctx->grayRow || !ctx->whiteRow) return;

  const int canvasY = pDraw->iY + pDraw->y;
  if (canvasY < 0 || canvasY >= ctx->srcHeight) {
    ctx->success = false;
    return;
  }

  processGapRowsUpTo(canvasY, *ctx);
  if (!ctx->success) return;

  memcpy(ctx->grayRow.get(), ctx->whiteRow.get(), static_cast<size_t>(ctx->srcWidth));
  updateGrayPalette(pDraw, *ctx);

  const uint8_t* pixels = pDraw->pPixels;
  const int startX = pDraw->iX;
  const int endX = startX + pDraw->iWidth;
  const bool hasTransparency = pDraw->ucHasTransparency != 0;
  const uint8_t transparent = pDraw->ucTransparent;

  for (int srcX = startX, i = 0; srcX < endX && srcX < ctx->srcWidth; ++srcX, ++i) {
    if (srcX < 0) continue;
    const uint8_t index = pixels[i];
    if (hasTransparency && index == transparent) continue;
    ctx->grayRow[srcX] = ctx->grayPalette[index];
  }

  renderCanvasRow(ctx->grayRow.get(), canvasY, *ctx);
  ctx->nextExpectedSrcY = canvasY + 1;
}

bool readGifInfoFromPath(const std::string& imagePath, GifBasicInfo& info) {
  HalFile file;
  if (!Storage.openFileForRead("GIF", imagePath, file)) return false;
  const bool ok = GifCommon::readBasicInfo(file, info);
  file.close();
  return ok;
}

constexpr size_t GIF_DECODER_APPROX_SIZE = sizeof(AnimatedGIF);
constexpr size_t MIN_FREE_HEAP_FOR_GIF = GIF_DECODER_APPROX_SIZE + 16 * 1024;

}  // namespace

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  GifBasicInfo info;
  if (!readGifInfoFromPath(imagePath, info)) {
    LOG_ERR("GIF", "Failed to read GIF dimensions: %s", imagePath.c_str());
    return false;
  }
  out.width = static_cast<int16_t>(info.canvasWidth);
  out.height = static_cast<int16_t>(info.canvasHeight);
  return true;
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("GIF", "Decoding GIF: %s", imagePath.c_str());

  GifBasicInfo info;
  if (!readGifInfoFromPath(imagePath, info)) {
    LOG_ERR("GIF", "Failed to parse GIF header: %s", imagePath.c_str());
    return false;
  }
  if (info.interlaced) {
    warnUnsupportedFeature("interlaced GIF", imagePath);
    return false;
  }
  if (info.canvasWidth > MAX_WIDTH) {
    LOG_ERR("GIF",
            "GIF canvas too wide for AnimatedGIF: canvas=%ux%u MAX_WIDTH=%d "
            "renderTarget=%dx%d file=%s",
            info.canvasWidth,
            info.canvasHeight,
            MAX_WIDTH,
            config.maxWidth,
            config.maxHeight,
            imagePath.c_str());
    return false;
  }
  if (!validateImageDimensions(info.canvasWidth, info.canvasHeight, "GIF")) {
    return false;
  }

  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_GIF) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_GIF);
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder");
    return false;
  }

  GifContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.srcWidth = info.canvasWidth;
  ctx.srcHeight = info.canvasHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = static_cast<float>(ctx.dstWidth) / ctx.srcWidth;
  } else {
    const float scaleX = static_cast<float>(config.maxWidth) / ctx.srcWidth;
    const float scaleY = static_cast<float>(config.maxHeight) / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;
    ctx.dstWidth = static_cast<int>(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = static_cast<int>(ctx.srcHeight * ctx.scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) {
    LOG_ERR("GIF", "Degenerate output dimensions %dx%d for %s", ctx.dstWidth, ctx.dstHeight, imagePath.c_str());
    return false;
  }

  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  if (!ctx.grayRow || !ctx.whiteRow) {
    LOG_ERR("GIF", "OOM: GIF row buffers (%d bytes each)", ctx.srcWidth);
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  gif->begin(GIF_PALETTE_RGB888);
  gif->setDrawType(GIF_DRAW_RAW);
  LOG_DBG("GIF",
          "GIF header: canvas=%ux%u frame=%ux%u frameOffset=%u,%u interlaced=%s "
          "renderTarget=%dx%d MAX_WIDTH=%d freeHeap=%u file=%s",
          info.canvasWidth,
          info.canvasHeight,
          info.frameWidth,
          info.frameHeight,
          info.frameX,
          info.frameY,
          info.interlaced ? "yes" : "no",
          config.maxWidth,
          config.maxHeight,
          MAX_WIDTH,
          ESP.getFreeHeap(),
          imagePath.c_str());
  const int rcOpen = gif->open(imagePath.c_str(), gifOpenWithHandle, gifCloseWithHandle, gifReadWithHandle,
                               gifSeekWithHandle, gifDrawCallback);
  const ScopedCleanup cleanup{[&gif]() { gif->close(); }};
  if (rcOpen != 1) {
    const int err = gif->getLastError();
    LOG_ERR("GIF",
            "AnimatedGIF open failed: err=%d canvas=%ux%u MAX_WIDTH=%d file=%s",
            err,
            info.canvasWidth,
            info.canvasHeight,
            MAX_WIDTH,
            imagePath.c_str());
    return false;
  }

  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("GIF", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  int delayMs = 0;
  const int frameState = gif->playFrame(false, &delayMs, &ctx);
  if (!ctx.success || (gif->getLastError() != GIF_SUCCESS && gif->getLastError() != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed (state=%d, err=%d)", frameState, gif->getLastError());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  processGapRowsUpTo(ctx.srcHeight, ctx);
  if (!ctx.success) {
    LOG_ERR("GIF", "GIF post-process failed: %s", imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  if (frameState > 0) {
    warnUnsupportedFeature("animation (using first frame only)", imagePath);
  }

  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasGifExtension(extension);
}