#include "GifToBmpConverter.h"

#include <AnimatedGIF.h>
#include <Arduino.h>
#include <GifCommon.h>
#include <GifImageLayout.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "BmpStreamWriter.h"

namespace {

constexpr uint16_t MAX_CANVAS_HEIGHT = 3072;

struct GifBmpContext {
  int srcWidth{0};
  int srcHeight{0};
  GifCommon::ImageLayout layout;
  BmpStreamWriter output;
  GrayRowScaler scaler;
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  bool success{true};
  uint32_t lastYieldMs{0};
};

static HalFile* s_gifFile = nullptr;

void* bmpGifOpen(const char* /*filename*/, int32_t* size) {
  if (!s_gifFile || !*s_gifFile) return nullptr;
  s_gifFile->seek(0);
  *size = static_cast<int32_t>(s_gifFile->size());
  return s_gifFile;
}

void bmpGifClose(void* /*handle*/) {}

int32_t bmpGifRead(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  if (!s_gifFile || !*s_gifFile) return 0;
  int32_t bytesRead = s_gifFile->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t bmpGifSeek(GIFFILE* pFile, int32_t position) {
  if (!s_gifFile || !*s_gifFile) return -1;
  if (position < 0) position = 0;
  if (position >= pFile->iSize) position = pFile->iSize - 1;
  if (!s_gifFile->seek(position)) return -1;
  pFile->iPos = position;
  return position;
}

void updateGrayPalette(const GIFDRAW* pDraw, GifBmpContext& ctx) {
  const uint8_t* palette24 = pDraw->pPalette24;
  if (palette24 == nullptr || palette24 == ctx.activePalette24) return;

  ctx.activePalette24 = palette24;
  for (int i = 0; i < 256; ++i) {
    const uint8_t* pixel = &palette24[i * 3];
    ctx.grayPalette[i] = static_cast<uint8_t>((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
  }
}

void processCanvasRow(const uint8_t* grayRow, int srcY, GifBmpContext& ctx) {
  if (!ctx.success || srcY < ctx.layout.sourceY || srcY >= ctx.layout.sourceY + ctx.layout.sourceHeight) return;
  ctx.success = ctx.scaler.writeRow(grayRow + ctx.layout.sourceX, srcY - ctx.layout.sourceY);
}

void processGapRowsUpTo(int canvasY, GifBmpContext& ctx) {
  while (ctx.success && ctx.nextExpectedSrcY < canvasY && ctx.nextExpectedSrcY < ctx.srcHeight) {
    processCanvasRow(ctx.whiteRow.get(), ctx.nextExpectedSrcY, ctx);
    ctx.nextExpectedSrcY++;
  }
}

void gifBmpDrawCallback(GIFDRAW* pDraw) {
  auto* ctx = reinterpret_cast<GifBmpContext*>(pDraw->pUser);
  if (!ctx || !ctx->success || !ctx->grayRow || !ctx->whiteRow) return;

  const uint32_t now = millis();
  if (now - ctx->lastYieldMs >= 250) {
    ctx->lastYieldMs = now;
    vTaskDelay(1);
  }

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

  processCanvasRow(ctx->grayRow.get(), canvasY, *ctx);
  ctx->nextExpectedSrcY = canvasY + 1;
}

constexpr size_t GIF_DECODER_APPROX_SIZE = sizeof(AnimatedGIF);
constexpr size_t MIN_FREE_HEAP = GIF_DECODER_APPROX_SIZE + 32 * 1024;

}  // namespace

bool GifToBmpConverter::gifFileToBmpStreamInternal(HalFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop, bool originalThresholds) {
  LOG_DBG("GIF", "Converting GIF to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  GifBasicInfo info;
  if (!GifCommon::readBasicInfo(gifFile, info)) {
    LOG_ERR("GIF", "Invalid GIF header");
    return false;
  }
  if (info.interlaced) {
    LOG_ERR("GIF", "Interlaced GIFs are not supported for BMP conversion");
    return false;
  }
  if (info.canvasWidth == 0 || info.canvasHeight == 0 || info.canvasWidth > MAX_WIDTH ||
      info.canvasHeight > MAX_CANVAS_HEIGHT) {
    LOG_ERR("GIF", "Image too large or zero (%ux%u)", info.canvasWidth, info.canvasHeight);
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder");
    return false;
  }

  auto ctxPtr = makeUniqueNoThrow<GifBmpContext>();
  if (!ctxPtr) {
    LOG_ERR("GIF", "OOM: GIF BMP context");
    return false;
  }
  GifBmpContext& ctx = *ctxPtr;
  ctx.srcWidth = info.canvasWidth;
  ctx.srcHeight = info.canvasHeight;

  if (!GifCommon::calculateLayout(ctx.srcWidth, ctx.srcHeight, targetWidth, targetHeight, crop, ctx.layout)) {
    LOG_ERR("GIF", "Invalid GIF output dimensions: %dx%d", targetWidth, targetHeight);
    return false;
  }
  const uint32_t bitsPerPixel = oneBit ? 1 : 2;
  const uint32_t rowBytes = ((static_cast<uint32_t>(ctx.layout.width) * bitsPerPixel + 31) / 32) * 4;
  const uint32_t headerBytes = 54 + (1u << bitsPerPixel) * 4;
  if (static_cast<uint64_t>(rowBytes) * ctx.layout.height + headerBytes > GifCommon::MAX_BMP_BYTES) {
    LOG_ERR("GIF", "GIF BMP exceeds output byte limit");
    return false;
  }

  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  if (!ctx.grayRow || !ctx.whiteRow) {
    LOG_ERR("GIF", "OOM: GIF BMP row buffers");
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  if (!ctx.output.begin(bmpOut, ctx.layout.width, ctx.layout.height, oneBit, originalThresholds) ||
      !ctx.scaler.begin(ctx.layout.sourceWidth, ctx.layout.sourceHeight, ctx.output)) {
    return false;
  }

  s_gifFile = &gifFile;
  gif->begin(GIF_PALETTE_RGB888);
  gif->setDrawType(GIF_DRAW_RAW);
  const int rcOpen = gif->open("", bmpGifOpen, bmpGifClose, bmpGifRead, bmpGifSeek, gifBmpDrawCallback);
  const ScopedCleanup cleanup{[]() { s_gifFile = nullptr; }};
  const ScopedCleanup gifCleanup{[&gif]() { gif->close(); }};
  if (rcOpen != 1) {
    LOG_ERR("GIF", "Failed to open GIF (err=%d)", gif->getLastError());
    return false;
  }

  int delayMs = 0;
  ctx.lastYieldMs = millis();
  const int frameState = gif->playFrame(false, &delayMs, &ctx);
  if (!ctx.success || (gif->getLastError() != GIF_SUCCESS && gif->getLastError() != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed (state=%d, err=%d)", frameState, gif->getLastError());
    return false;
  }

  processGapRowsUpTo(ctx.srcHeight, ctx);
  if (!ctx.success) {
    LOG_ERR("GIF", "GIF BMP post-process failed");
    return false;
  }

  if (frameState > 0) {
    LOG_INF("GIF", "Animated GIF detected - using first frame only for BMP conversion");
  }

  LOG_DBG("GIF", "Successfully converted GIF to BMP");
  return true;
}

bool GifToBmpConverter::gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop, bool originalThresholds) {
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetWidth, targetHeight, false, crop, originalThresholds);
}

bool GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetMaxWidth, targetMaxHeight, true, false);
}
