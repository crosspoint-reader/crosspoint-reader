#include "GifToBmpConverter.h"

#include <AnimatedGIF.h>
#include <GifCommon.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "BitmapHelpers.h"

namespace {

constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 3) / 4 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 14 + 40 + paletteSize);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 8);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 256);
  write32(bmpOut, 256);

  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(0));
  }
}

void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 62);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 1);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 2);
  write32(bmpOut, 2);

  uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t value : palette) {
    bmpOut.write(value);
  }
}

void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 2);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);

  uint8_t palette[16] = {0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x00,
                         0xAA, 0xAA, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t value : palette) {
    bmpOut.write(value);
  }
}

struct GifBmpContext {
  Print* bmpOut{nullptr};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  bool oneBit{false};
  bool needsScaling{false};
  uint32_t scaleX_fp{1u << 16};
  uint32_t scaleY_fp{1u << 16};
  int bytesPerRow{0};
  int currentOutY{0};
  uint32_t nextOutYSrcStart{0};
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> scaledGrayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  std::unique_ptr<uint8_t[]> rowBuffer;
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint16_t[]> rowCount;
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;
  bool success{true};
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

void emitOutputRow(const uint8_t* row, GifBmpContext& ctx, int outY) {
  memset(ctx.rowBuffer.get(), 0, static_cast<size_t>(ctx.bytesPerRow));

  if (USE_8BIT_OUTPUT && !ctx.oneBit) {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      ctx.rowBuffer[x] = adjustPixel(row[x]);
    }
  } else if (ctx.oneBit) {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      const uint8_t bit = ctx.atkinson1BitDitherer
                              ? ctx.atkinson1BitDitherer->processPixel(row[x], x)
                              : quantize1bit(row[x], x, outY);
      const int byteIndex = x / 8;
      const int bitOffset = 7 - (x % 8);
      ctx.rowBuffer[byteIndex] |= (bit << bitOffset);
    }
    if (ctx.atkinson1BitDitherer) ctx.atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      const uint8_t gray = adjustPixel(row[x]);
      uint8_t twoBit;
      if (ctx.atkinsonDitherer) {
        twoBit = ctx.atkinsonDitherer->processPixel(gray, x);
      } else if (ctx.fsDitherer) {
        twoBit = ctx.fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      const int byteIndex = (x * 2) / 8;
      const int bitOffset = 6 - ((x * 2) % 8);
      ctx.rowBuffer[byteIndex] |= (twoBit << bitOffset);
    }
    if (ctx.atkinsonDitherer)
      ctx.atkinsonDitherer->nextRow();
    else if (ctx.fsDitherer)
      ctx.fsDitherer->nextRow();
  }

  ctx.bmpOut->write(ctx.rowBuffer.get(), ctx.bytesPerRow);
}

void processCanvasRow(const uint8_t* grayRow, int srcY, GifBmpContext& ctx) {
  if (!ctx.success) return;

  if (!ctx.needsScaling) {
    emitOutputRow(grayRow, ctx, srcY);
    return;
  }

  for (int outX = 0; outX < ctx.dstWidth; ++outX) {
    const int srcXStart = (static_cast<uint32_t>(outX) * ctx.scaleX_fp) >> 16;
    const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx.scaleX_fp) >> 16;

    int sum = 0;
    int count = 0;
    for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx.srcWidth; ++srcX) {
      sum += grayRow[srcX];
      count++;
    }
    if (count == 0 && srcXStart < ctx.srcWidth) {
      sum = grayRow[srcXStart];
      count = 1;
    }

    ctx.rowAccum[outX] += sum;
    ctx.rowCount[outX] += count;
  }

  const uint32_t srcY_fp = static_cast<uint32_t>(srcY + 1) << 16;
  while (srcY_fp >= ctx.nextOutYSrcStart && ctx.currentOutY < ctx.dstHeight) {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      ctx.scaledGrayRow[x] = static_cast<uint8_t>((ctx.rowCount[x] > 0) ? (ctx.rowAccum[x] / ctx.rowCount[x]) : 255);
    }
    emitOutputRow(ctx.scaledGrayRow.get(), ctx, ctx.currentOutY);
    ctx.currentOutY++;
    ctx.nextOutYSrcStart = static_cast<uint32_t>(ctx.currentOutY + 1) * ctx.scaleY_fp;
    if (srcY_fp >= ctx.nextOutYSrcStart) {
      continue;
    }
    memset(ctx.rowAccum.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
    memset(ctx.rowCount.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint16_t));
  }
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
                                                   bool oneBit, bool crop) {
  LOG_DBG("GIF", "Converting GIF to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth,
          targetHeight);

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
  if (info.canvasWidth == 0 || info.canvasHeight == 0 || info.canvasWidth > MAX_WIDTH || info.canvasHeight > 3072) {
    LOG_ERR("GIF", "Image too large or zero (%ux%u)", info.canvasWidth, info.canvasHeight);
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder");
    return false;
  }

  GifBmpContext ctx;
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = info.canvasWidth;
  ctx.srcHeight = info.canvasHeight;
  ctx.oneBit = oneBit;

  int outWidth = ctx.srcWidth;
  int outHeight = ctx.srcHeight;
  if (targetWidth > 0 && targetHeight > 0 && (ctx.srcWidth != targetWidth || ctx.srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / ctx.srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / ctx.srcHeight;
    float scale = crop ? ((scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight)
                       : ((scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight);
    outWidth = static_cast<int>(ctx.srcWidth * scale);
    outHeight = static_cast<int>(ctx.srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;
    ctx.scaleX_fp = (static_cast<uint32_t>(ctx.srcWidth) << 16) / outWidth;
    ctx.scaleY_fp = (static_cast<uint32_t>(ctx.srcHeight) << 16) / outHeight;
    ctx.needsScaling = true;
  }
  ctx.dstWidth = outWidth;
  ctx.dstHeight = outHeight;
  ctx.nextOutYSrcStart = ctx.scaleY_fp;

  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, ctx.dstWidth, ctx.dstHeight);
    ctx.bytesPerRow = (ctx.dstWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, ctx.dstWidth, ctx.dstHeight);
    ctx.bytesPerRow = (ctx.dstWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, ctx.dstWidth, ctx.dstHeight);
    ctx.bytesPerRow = (ctx.dstWidth * 2 + 31) / 32 * 4;
  }

  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.rowBuffer = makeUniqueNoThrow<uint8_t[]>(ctx.bytesPerRow);
  if (!ctx.grayRow || !ctx.whiteRow || !ctx.rowBuffer) {
    LOG_ERR("GIF", "OOM: GIF BMP row buffers");
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  if (ctx.needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(ctx.dstWidth);
    ctx.rowCount = makeUniqueNoThrow<uint16_t[]>(ctx.dstWidth);
    ctx.scaledGrayRow = makeUniqueNoThrow<uint8_t[]>(ctx.dstWidth);
    if (!ctx.rowAccum || !ctx.rowCount || !ctx.scaledGrayRow) {
      LOG_ERR("GIF", "OOM: GIF scaling accumulators");
      return false;
    }
    memset(ctx.rowAccum.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
    memset(ctx.rowCount.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint16_t));
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(ctx.dstWidth);
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("GIF", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(ctx.dstWidth);
      if (!ctx.atkinsonDitherer) {
        LOG_ERR("GIF", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(ctx.dstWidth);
      if (!ctx.fsDitherer) {
        LOG_ERR("GIF", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
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

bool GifToBmpConverter::gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop) {
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetWidth, targetHeight, false, crop);
}

bool GifToBmpConverter::gifFileToBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight) {
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}