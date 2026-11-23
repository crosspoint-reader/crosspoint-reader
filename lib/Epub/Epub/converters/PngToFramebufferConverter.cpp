#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageOpsConfig.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>

#if CROSSPOINT_ACCELERATED_IMAGES
#include "PortraitImageRows.h"
#include "PortraitImageWriter.h"
#endif

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

#if CROSSPOINT_ACCELERATED_IMAGES
enum class GrayRowLayout {
  Source,
  Destination,
  PackedIndices,
  Indices,
};

enum class IndexedPaletteState { Empty, Ready, Disabled };
#endif

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  PNG* decoder{nullptr};
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int cropLeft{0};
  int cropTop{0};
  int visibleWidth{0};
  int visibleHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates
  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
  uint8_t* alphaLineBuffer{nullptr};
  uint32_t lastYieldMs{0};  // throttle state for yieldDuringDecode()

#if CROSSPOINT_ACCELERATED_IMAGES
  std::unique_ptr<uint8_t[]> indexedStorage;
  IndexedPaletteState indexedState{IndexedPaletteState::Empty};
  int indexedBits{0};
  bool indexedHasAlpha{false};
  bool indexedRetainAlpha{false};
  portraitImage::LevelRows levelRows;
  int batchY{0};
  int batchRows{0};
  DirectPixelWriter batchPixels{};

  void flushBatch() {
    if (batchRows == 0) return;
    portraitImage::writeLevels(batchPixels, levelRows.data(), levelRows.stride(), batchRows, levelRows.width(),
                               config->x, batchY);
    batchRows = 0;
  }
#endif
};

#if CROSSPOINT_ACCELERATED_IMAGES
bool canBatchPortrait(const PngContext& ctx, const DirectPixelWriter& pixels) {
  return ctx.config->useDithering && !ctx.config->preserveAlpha && !pixels.ditherBw && pixels.fb &&
         pixels.phyXStepX == 0 && (pixels.phyYStepX == 1 || pixels.phyYStepX == -1) &&
         (pixels.phyXStepY == 1 || pixels.phyXStepY == -1) && ctx.config->x >= 0 && ctx.config->y >= 0 &&
         ctx.config->x < ctx.screenWidth && ctx.config->y < ctx.screenHeight && ctx.dstWidth > 0 && ctx.dstHeight > 0;
}

bool sameBatchTarget(const DirectPixelWriter& a, const DirectPixelWriter& b) {
  return a.fb == b.fb && a.mode == b.mode && a.absolute == b.absolute && a.ditherBw == b.ditherBw &&
         a.displayWidthBytes == b.displayWidthBytes && a.originY == b.originY && a.clipRows == b.clipRows &&
         a.phyXBase == b.phyXBase && a.phyYBase == b.phyYBase && a.phyXStepX == b.phyXStepX &&
         a.phyYStepX == b.phyYStepX && a.phyXStepY == b.phyXStepY && a.phyYStepY == b.phyYStepY;
}

void allocatePortraitLevels(PngContext& ctx) {
  DirectPixelWriter pixels;
  pixels.init(*ctx.renderer);
  if (!canBatchPortrait(ctx, pixels)) return;
  if (ctx.decoder &&
      (ctx.decoder->getPixelType() == PNG_PIXEL_GRAY_ALPHA || ctx.decoder->getPixelType() == PNG_PIXEL_TRUECOLOR_ALPHA))
    return;
  ctx.levelRows.allocate(std::min(ctx.dstWidth, ctx.screenWidth - ctx.config->x), "PNG");
}

GrayRowLayout convertOpaqueRgbRow(PngContext& ctx, const uint8_t* rgb) {
  const int width = ctx.levelRows.width();
  if (width > ctx.srcWidth) {
    // On horizontal upscales every source sample in the selected prefix is used.
    const size_t count = (static_cast<uint64_t>(width - 1) * ctx.visibleWidth) / ctx.dstWidth + 1;
    imageops::rgb24ToGraySpan(rgb + ctx.cropLeft * 3, ctx.grayLineBuffer + ctx.cropLeft, count);
    return GrayRowLayout::Source;
  }
  if (ctx.visibleWidth == ctx.dstWidth) {
    imageops::rgb24ToGraySpan(rgb + ctx.cropLeft * 3, ctx.grayLineBuffer, width);
    return GrayRowLayout::Destination;
  }
  int srcX = ctx.cropLeft;
  int error = 0;
  for (int x = 0; x < width;) {
    alignas(4) uint8_t packed[48];
    const int count = std::min(16, width - x);
    for (int i = 0; i < count; ++i) {
      memcpy(packed + i * 3, rgb + srcX * 3, 3);
      error += ctx.visibleWidth;
      while (error >= ctx.dstWidth) {
        error -= ctx.dstWidth;
        ++srcX;
      }
    }
    imageops::rgb24ToGraySpan(packed, ctx.grayLineBuffer + x, count);
    x += count;
  }
  return GrayRowLayout::Destination;
}
void preparePortraitRow(PngContext& ctx, const DirectPixelWriter& pixels, DirectCacheWriter* cache, int outY,
                        GrayRowLayout layout, [[maybe_unused]] const PNGDRAW& draw) {
  if (ctx.batchRows != 0 && outY != ctx.batchY + ctx.batchRows) ctx.flushBatch();
  if (ctx.batchRows == 0) {
    ctx.batchY = outY;
    ctx.batchPixels = pixels;
  }
  auto* row = ctx.levelRows.row(ctx.batchRows);
  const int width = ctx.levelRows.width();
  memset(row, 255, ctx.levelRows.stride());
  switch (layout) {
    case GrayRowLayout::Destination:
      memcpy(row, ctx.grayLineBuffer, width);
      break;
    case GrayRowLayout::Source:
      if (ctx.visibleWidth == ctx.dstWidth) {
        memcpy(row, ctx.grayLineBuffer + ctx.cropLeft, width);
      } else {
        int srcX = ctx.cropLeft;
        int error = 0;
        for (int x = 0; x < width; ++x) {
          row[x] = ctx.grayLineBuffer[srcX];
          error += ctx.visibleWidth;
          while (error >= ctx.dstWidth) {
            error -= ctx.dstWidth;
            ++srcX;
          }
        }
      }
      break;
    case GrayRowLayout::PackedIndices:
      imageops::unpackIndexSpan(draw.pPixels, row, width, static_cast<imageops::IndexDepth>(draw.iBpp), ctx.cropLeft);
      for (int x = 0; x < width; ++x) row[x] = ctx.indexedStorage[row[x]];
      break;
    case GrayRowLayout::Indices: {
      int srcX = ctx.cropLeft;
      int error = 0;
      for (int x = 0; x < width; ++x) {
        row[x] = ctx.indexedStorage[ctx.grayLineBuffer[srcX]];
        error += ctx.visibleWidth;
        while (error >= ctx.dstWidth) {
          error -= ctx.dstWidth;
          ++srcX;
        }
      }
      break;
    }
  }
  portraitImage::finishDitheredRow(row, width, ctx.config->x, outY, cache);
  ++ctx.batchRows;
  if (ctx.batchRows == portraitImage::batchRows(pixels, ctx.batchY, 8)) ctx.flushBatch();
}
#endif

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
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

int packedRowBytes(int srcWidth, int bitsPerSample) { return (srcWidth * bitsPerSample + 7) / 8; }

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

uint8_t alphaThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

#if CROSSPOINT_ACCELERATED_IMAGES
bool useIndexedPalette(PngContext& ctx, const PNGDRAW& draw) {
  if (!isSupportedBitDepth(PNG_PIXEL_INDEXED, draw.iBpp)) {
    ctx.indexedState = IndexedPaletteState::Disabled;
    return false;
  }
  const unsigned entries = 1u << draw.iBpp;
  const bool hasAlpha = draw.iHasAlpha != 0;
  const bool retainAlpha = ctx.alphaLineBuffer != nullptr;
  switch (ctx.indexedState) {
    case IndexedPaletteState::Disabled:
      return false;
    case IndexedPaletteState::Ready: {
      const auto* snapshot = ctx.indexedStorage.get() + 512;
      if (!draw.pPalette || ctx.indexedBits != draw.iBpp || ctx.indexedHasAlpha != hasAlpha ||
          ctx.indexedRetainAlpha != retainAlpha || memcmp(snapshot, draw.pPalette, entries * 3) != 0 ||
          (hasAlpha && memcmp(snapshot + 768, draw.pPalette + 768, entries) != 0)) {
        ctx.indexedState = IndexedPaletteState::Disabled;
        return false;
      }
      return true;
    }
    case IndexedPaletteState::Empty: {
      if (!draw.pPalette) return false;
      // Attempt once; OOM and later palette changes keep scalar conversion.
      ctx.indexedState = IndexedPaletteState::Disabled;
      constexpr size_t WORKSPACE_BYTES = 512 + 1024;
      ctx.indexedStorage = makeUniqueNoThrow<uint8_t[]>(WORKSPACE_BYTES);
      if (!ctx.indexedStorage) {
        LOG_ERR("PNG", "Failed to allocate indexed palette (%u bytes), using scalar conversion",
                static_cast<unsigned>(WORKSPACE_BYTES));
        return false;
      }
      auto* table = ctx.indexedStorage.get();
      imageops::prepareIndexedPalette(draw.pPalette, static_cast<imageops::IndexDepth>(draw.iBpp), hasAlpha,
                                      retainAlpha, table, table + 256);
      auto* snapshot = table + 512;
      memcpy(snapshot, draw.pPalette, entries * 3);
      if (hasAlpha) memcpy(snapshot + 768, draw.pPalette + 768, entries);
      ctx.indexedBits = draw.iBpp;
      ctx.indexedHasAlpha = hasAlpha;
      ctx.indexedRetainAlpha = retainAlpha;
      ctx.indexedState = IndexedPaletteState::Ready;
      return true;
    }
  }
  return false;
}

GrayRowLayout convertIndexedRow(PngContext& ctx, const PNGDRAW& draw, bool batch) {
  const auto depth = static_cast<imageops::IndexDepth>(draw.iBpp);
  if (batch) {
    if (ctx.visibleWidth == ctx.dstWidth) return GrayRowLayout::PackedIndices;
    imageops::unpackIndexSpan(draw.pPixels, ctx.grayLineBuffer + ctx.cropLeft, ctx.visibleWidth, depth, ctx.cropLeft);
    return GrayRowLayout::Indices;
  }
  imageops::unpackIndexSpan(draw.pPixels, ctx.grayLineBuffer, ctx.srcWidth, depth);
  const auto* table = ctx.indexedStorage.get();
  for (int x = 0; x < ctx.srcWidth; ++x) {
    const uint8_t index = ctx.grayLineBuffer[x];
    ctx.grayLineBuffer[x] = table[index];
    if (ctx.alphaLineBuffer) ctx.alphaLineBuffer[x] = table[256 + index];
  }
  return GrayRowLayout::Source;
}
#endif

// Convert an entire source line to grayscale and optionally retain alpha.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha, uint32_t transparentColor, uint8_t* alphaLine) {
  const auto store = [grayLine, alphaLine](const int x, const uint8_t gray, const uint8_t alpha) {
    grayLine[x] = alphaLine ? gray : static_cast<uint8_t>((gray * alpha + 255u * (255u - alpha)) / 255u);
    if (alphaLine) alphaLine[x] = alpha;
  };

  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8 && !hasAlpha && !alphaLine) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          const uint8_t sample = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t gray = expandSampleToByte(sample, bitsPerSample);
          store(x, gray, hasAlpha && sample == static_cast<uint8_t>(transparentColor) ? 0 : 255);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        const uint32_t color = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        store(x, gray, hasAlpha && color == transparentColor ? 0 : 255);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        for (int x = 0; x < width; x++) {
          const uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t* p = &palette[idx * 3];
          const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          store(x, gray, hasAlpha ? palette[768 + idx] : 255);
        }
      } else {
        for (int x = 0; x < width; x++) {
          store(x, expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample), 255);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        store(x, pPixels[x * 2], pPixels[x * 2 + 1]);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        store(x, gray, p[3]);
      }
      break;

    default:
      memset(grayLine, 128, width);
      if (alphaLine) memset(alphaLine, 255, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;
  if (srcY < ctx->cropTop || srcY >= ctx->cropTop + ctx->visibleHeight) return 1;
  const int visibleSrcY = srcY - ctx->cropTop;

  // Map source rows with the exact output-height ratio. During downscaling,
  // multiple source rows can select the same output row; during upscaling, one
  // source row must be repeated across every output row in its range. Emitting
  // only the first row of an upscale leaves zero-filled (black) gaps in the
  // streamed pixel cache.
  int firstDstY = (visibleSrcY * ctx->dstHeight) / ctx->visibleHeight;
  int endDstY = firstDstY + 1;
  if (ctx->dstHeight > ctx->visibleHeight) {
    endDstY = ((visibleSrcY + 1) * ctx->dstHeight) / ctx->visibleHeight;
  }

  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;

  // PNGdec parses tRNS while decoding, after open() returns, so query the
  // transparent color here rather than caching its pre-decode value.
  const uint32_t transparentColor = ctx->decoder ? ctx->decoder->getTransparentColor() : 0;

  // Render scaled rows using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;

  // Pre-compute orientation and render-mode state once per callback.
  DirectPixelWriter pw;
  pw.init(*ctx->renderer);

#if CROSSPOINT_ACCELERATED_IMAGES
  if (ctx->batchRows != 0 && !sameBatchTarget(ctx->batchPixels, pw)) ctx->flushBatch();
  const bool batch = ctx->levelRows.data() && canBatchPortrait(*ctx, pw) && !pDraw->iHasAlpha &&
                     pDraw->iPixelType != PNG_PIXEL_GRAY_ALPHA && pDraw->iPixelType != PNG_PIXEL_TRUECOLOR_ALPHA;
  if (!batch) ctx->flushBatch();
  GrayRowLayout grayLayout = GrayRowLayout::Source;

  bool converted = false;
  if (pDraw->iPixelType == PNG_PIXEL_INDEXED && useIndexedPalette(*ctx, *pDraw)) {
    grayLayout = convertIndexedRow(*ctx, *pDraw, batch);
    converted = true;
  }
  if (!converted && batch && pDraw->iPixelType == PNG_PIXEL_TRUECOLOR && pDraw->iBpp == 8 && !ctx->alphaLineBuffer) {
    grayLayout = convertOpaqueRgbRow(*ctx, pDraw->pPixels);
    converted = true;
  }
  if (!converted) {
#endif
    convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                      pDraw->iHasAlpha, transparentColor, ctx->alphaLineBuffer);
#if CROSSPOINT_ACCELERATED_IMAGES
  }
#endif

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    pw.beginRow(outY);

    // The cache streams to disk one row at a time. Flushing rows below this one
    // (PNGdec delivers scanlines top to bottom) repositions the single-row band.
    // A flush failure stops caching for the rest of the decode so we never write
    // past the band buffer; finalize() then drops the partial file.
    bool caching = ctx->caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx->cache.advanceTo(dstY)) {
        caching = false;
        ctx->caching = false;
      } else {
        cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
        cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
      }
    }

#if CROSSPOINT_ACCELERATED_IMAGES
    if (batch) {
      preparePortraitRow(*ctx, pw, caching ? &cw : nullptr, outY, grayLayout, *pDraw);
      continue;
    }
#endif

    int srcX = ctx->cropLeft;
    int error = 0;

    for (int dstX = 0; dstX < dstWidth; dstX++) {
      int outX = outXBase + dstX;
      if (outX < screenWidth) {
        const uint8_t alpha = ctx->alphaLineBuffer ? ctx->alphaLineBuffer[srcX] : 255;
        if (alpha >= 8 && alpha > alphaThreshold4x4(outX, outY)) {
          uint8_t gray = ctx->grayLineBuffer[srcX];

          uint8_t ditheredGray;
          if (useDithering) {
            ditheredGray = applyBayerDither4Level(gray, outX, outY);
          } else {
            ditheredGray = gray >> 6;
          }
          pw.writePixel(outX, ditheredGray, ctx->alphaLineBuffer != nullptr);
          if (caching) cw.writePixel(outX, ditheredGray);
        }
      }

      // Bresenham-style stepping: advance srcX based on ratio visibleWidth/dstWidth
      error += ctx->visibleWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
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

  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder for dimensions");
    return false;
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};

  if (rc != 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %d", rc);
    return false;
  }

  return validateAndStoreDimensions(png->getWidth(), png->getHeight(), out, "PNG");
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
  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.decoder = png.get();
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(png->getWidth(), png->getHeight(), sourceDimensions, "PNG")) return false;

  // Calculate output dimensions
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;
  ctx.cropLeft = static_cast<int>(ctx.srcWidth * std::clamp(config.sourceCropX, 0.0f, 0.99f) / 2.0f);
  ctx.cropTop = static_cast<int>(ctx.srcHeight * std::clamp(config.sourceCropY, 0.0f, 0.99f) / 2.0f);
  ctx.visibleWidth = ctx.srcWidth - 2 * ctx.cropLeft;
  ctx.visibleHeight = ctx.srcHeight - 2 * ctx.cropTop;
  if (ctx.visibleWidth <= 0 || ctx.visibleHeight <= 0) {
    LOG_ERR("PNG", "PNG crop leaves no visible pixels");
    return false;
  }

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.visibleWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.visibleWidth;
    float scaleY = (float)config.maxHeight / ctx.visibleHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.visibleWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.visibleHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  LOG_DBG("PNG", "PNG %dx%d (visible %dx%d) -> %dx%d (scale %.2f), type: %d, bpp: %d", ctx.srcWidth, ctx.srcHeight,
          ctx.visibleWidth, ctx.visibleHeight, ctx.dstWidth, ctx.dstHeight, ctx.scale, pixelType, bitsPerSample);

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR(
        "PNG",
        "PNG row buffer too small: need %d bytes for width=%d type=%d bpp=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
        requiredInternal, ctx.srcWidth, pixelType, bitsPerSample, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    return false;
  }

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    return false;
  }

  // The converter expands each source row to 8-bit grayscale before dithering,
  // so this scratch buffer is sized by source pixels even when PNGdec reads a
  // packed 1/2/4-bit row internally.
  constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = PNG_MAX_BUFFERED_PIXELS / 2;
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for width=%d, max=%u", static_cast<unsigned>(grayBufSize),
            ctx.srcWidth, static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    return false;
  }

  // tRNS chunks are parsed during decode(), so hasAlpha() is not reliable yet
  // for color-key and indexed transparency. Reserve the alpha line whenever
  // the caller requests preservation; opaque pixels simply store alpha 255.
  const bool retainAlpha = config.preserveAlpha;
  const size_t lineBufferBytes = grayBufSize * (retainAlpha ? 2u : 1u);
  auto lineBuffers = makeUniqueNoThrow<uint8_t[]>(lineBufferBytes);
  if (!lineBuffers) {
    LOG_ERR("PNG", "Failed to allocate PNG line buffers (%u bytes)", static_cast<unsigned>(lineBufferBytes));
    return false;
  }
  ctx.grayLineBuffer = lineBuffers.get();
  ctx.alphaLineBuffer = retainAlpha ? ctx.grayLineBuffer + grayBufSize : nullptr;

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.preserveAlpha && !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

#if CROSSPOINT_ACCELERATED_IMAGES
  allocatePortraitLevels(ctx);
#endif

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  rc = png->decode(&ctx, 0);
#if CROSSPOINT_ACCELERATED_IMAGES
  // Match scalar partial-frame output even when decoding stops before a full batch.
  ctx.flushBatch();
#endif
  unsigned long decodeTime = millis() - decodeStart;

  ctx.grayLineBuffer = nullptr;
  ctx.alphaLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
