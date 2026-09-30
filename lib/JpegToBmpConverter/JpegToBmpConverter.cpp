#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "BmpStreamWriter.h"

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static HalFile* s_jpegFile = nullptr;
static uint8_t s_jpegIoSinceYield = 0;

static void yieldToIdle() { vTaskDelay(1); }

static void yieldDuringJpegIo() {
  if (++s_jpegIoSinceYield < 4) return;
  s_jpegIoSinceYield = 0;
  yieldToIdle();
}

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegIoSinceYield = 0;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  yieldDuringJpegIo();
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  yieldDuringJpegIo();
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  yieldDuringJpegIo();
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;
  BmpStreamWriter output;
  GrayRowScaler scaler;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  uint8_t blocksSinceYield;
  bool error;
};

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int outY) {
  if (!ctx->output.writeRow(srcRow, outY)) ctx->error = true;
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->scaler.writeRow(srcRow, y)) {
      ctx->error = true;
      return 0;
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth,
                                                     int targetHeight, bool oneBit, bool crop,
                                                     bool originalThresholds) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  s_jpegFile = &jpegFile;

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);
  // JPEGDEC forces progressive streams to JPEG_SCALE_EIGHTH in DecodeJPEG,
  // so callback coordinates and MCU buffering must use the reduced decode grid.
  const int decodedSrcWidth = progressiveDecode ? ((srcWidth + 7) >> 3) : srcWidth;
  const int decodedSrcHeight = progressiveDecode ? ((srcHeight + 7) >> 3) : srcHeight;

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);
  if (progressiveDecode) {
    LOG_DBG("JPG", "Progressive JPEG decode uses 1/8 source: %dx%d", decodedSrcWidth, decodedSrcHeight);
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  int outWidth;
  int outHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    outWidth = decodedSrcWidth;
    outHeight = decodedSrcHeight;
  } else if (!calculateBmpOutputSize(srcWidth, srcHeight, targetWidth, targetHeight, crop, outWidth, outHeight)) {
    return false;
  }

  const int scaleSrcWidth = decodedSrcWidth;
  const int scaleSrcHeight = decodedSrcHeight;
  const bool needsScaling = scaleSrcWidth != outWidth || scaleSrcHeight != outHeight;
  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  LOG_DBG("JPG", "Scaling source %dx%d (decode grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight, scaleSrcWidth,
          scaleSrcHeight, outWidth, outHeight, targetWidth, targetHeight);

  BmpConvertCtx ctx = {};
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.blocksSinceYield = 0;
  ctx.error = false;

  if (!ctx.output.begin(bmpOut, outWidth, outHeight, oneBit, originalThresholds)) return false;
  if (!smoothUpscale && !ctx.scaler.begin(scaleSrcWidth, scaleSrcHeight, ctx.output)) return false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, 0);

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop, bool originalThresholds) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop, originalThresholds);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
