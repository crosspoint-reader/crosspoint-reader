#include "PngToFramebufferConverter.h"

#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <PngStreamDecoder.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "../blocks/ImageBlock.h"  // image_scratch: pass-wide decode arena
#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

// PNG decode now runs on uzlib (PngStreamDecoder) instead of PNGdec. The PNGdec
// PNGIMAGE object was ~49.5 KB — larger than the 48 KB framebuffer the reader
// frees to make room for it — so `new PNG()` failed intermittently under heap
// fragmentation and images silently vanished. PngStreamDecoder needs only the
// DEFLATE window (≤32 KB, sized down for small images) plus a couple of scanline
// buffers, so it fits the freed framebuffer with margin.

namespace {

// Ditherer state, mirroring the modes the old PNGdec path supported:
//   monochromeOutput -> 1-bit Atkinson (reader images)
//   else             -> 4-level Bayer (sleep-screen grayscale planes), plus the
//                       optional error-diffusion ditherers behind the extension flag.
struct DitherState {
  const RenderConfig* config{nullptr};
  std::unique_ptr<Atkinson1BitDitherer> atkinson1Bit;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  std::unique_ptr<AtkinsonDitherer> atkinson4;
  std::unique_ptr<DiffusedBayerDitherer> bayerDiff;
#endif
};

// Map one grayscale sample to a 2-bit value (0..3). Called for every destination
// column — including off-screen ones — so error-diffusion state stays consistent
// across the row; only the framebuffer/cache write is bounds-guarded by the caller.
//
// `gray` arrives already level-corrected: the caller applies the tone curve once so that a
// companion sink (below) dithers the SAME sample rather than re-deriving it.
uint8_t ditherGray(DitherState& d, uint8_t gray, int localX, int outX, int outY) {
  if (d.atkinson1Bit) return d.atkinson1Bit->processPixel(gray, localX) ? 3 : 0;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (d.config->useDithering) {
    switch (d.config->ditherMode) {
      case ImageDitherMode::Atkinson:
        if (d.atkinson4) return d.atkinson4->processPixel(gray, localX);
        break;
      case ImageDitherMode::DiffusedBayer:
        if (d.bayerDiff) return d.bayerDiff->processPixel(gray, localX, outX, outY);
        break;
      default:
        break;
    }
  }
#endif
  return applyBayerDither4Level(gray, outX, outY);
}

void advanceDitherRow(DitherState& d) {
  if (d.atkinson1Bit) d.atkinson1Bit->nextRow();
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (d.atkinson4) d.atkinson4->nextRow();
  if (d.bayerDiff) d.bayerDiff->nextRow();
#endif
}

// A second rendition of the same decode, written to its own .pxc and never drawn.
//
// The reader wants two caches per image — 1-bit Atkinson for the BW plane, 4-level Bayer for
// the grayscale planes — and used to get them from two full decodes. They are the same 2 bpp
// format over the same source samples and (since in-book adaptive tone was dropped) the same
// tone, so they differ only in ditherer: one inflate can feed both. On the X4 cover that
// measured 5.72 s of the 14.93 s an uncached image page cost.
//
// Deliberately limited to the two plain ditherers. The error-diffusion modes behind
// ENABLE_IMAGE_DITHERING_EXTENSION stay primary-only: a companion is only ever asked for by
// the reader's BW+grey pair, and the extension modes are alternatives to the grey rendition,
// not a third variant anyone caches.
struct CompanionSink {
  // Non-null => 1-bit Atkinson; null => stateless ordered 4-level Bayer, which is why the
  // common case (BW primary + grey companion) costs no extra ditherer state at all.
  std::unique_ptr<Atkinson1BitDitherer> atkinson1Bit;
  PixelCache cache;
  DirectCacheWriter writer;
  bool caching{false};
  bool rowCaching{false};

  uint8_t dither(uint8_t gray, int localX, int outX, int outY) {
    if (atkinson1Bit) return atkinson1Bit->processPixel(gray, localX) ? 3 : 0;
    return applyBayerDither4Level(gray, outX, outY);
  }
  void nextRow() {
    if (atkinson1Bit) atkinson1Bit->nextRow();
  }
};

// Below this free heap we don't even start: the uzlib ring (≤32 KB) plus scanline
// buffers won't fit. begin() also fails gracefully if a specific malloc fails.
constexpr size_t PNG_DECODE_HEAP_FLOOR = 36 * 1024;

// The ring is the whole reason for the floor above, and PngStreamDecoder draws it from the pass
// arena when one is installed (setScratchArena -> the alloc at PngStreamDecoder.cpp). With an
// arena the heap only has to cover the decoder object, the cache band and the ditherer rows —
// roughly 9 KB measured — so charging the full 36 KB refuses decodes the heap can easily serve.
// This is what would otherwise make a borrowed framebuffer (free heap ~52 KB lower than the
// released path) unusable for image pages. See docs/memory-allocation-strategy.md §9.3.
//
// The residual floor keeps real headroom rather than dropping to the measured 9 KB: the band and
// ditherer scale with image width, and begin()'s per-allocation fallbacks are graceful but not
// free.
constexpr size_t PNG_DECODE_HEAP_FLOOR_WITH_ARENA = 16 * 1024;

// Worst case the arena is asked for on this path, in the order PngStreamDecoder::begin() takes
// them: two scanline buffers, then the ring (capped at 32 KB). Image width is unknown at the
// gate, so budget the same way WARM_PASS_SCRATCH_BYTES does. Asking for the whole worst case is
// deliberate — a partial fit sends the ring back to the heap, which is the block the floor
// exists for.
constexpr size_t PNG_ARENA_WORST_CASE_BYTES = 32 * 1024 + 2 * 4096;

size_t pngDecodeHeapFloor() {
  return image_scratch::canServe(PNG_ARENA_WORST_CASE_BYTES) ? PNG_DECODE_HEAP_FLOOR_WITH_ARENA : PNG_DECODE_HEAP_FLOOR;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* buf, const size_t len, ImageDimensions& out) {
  if (!buf || len < 24) return false;
  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(buf, kPngSig, 8) != 0) return false;
  if (buf[12] != 'I' || buf[13] != 'H' || buf[14] != 'D' || buf[15] != 'R') return false;
  const uint32_t w =
      ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) | ((uint32_t)buf[18] << 8) | (uint32_t)buf[19];
  const uint32_t h =
      ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) | ((uint32_t)buf[22] << 8) | (uint32_t)buf[23];
  if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
  out.width = static_cast<int16_t>(w);
  out.height = static_cast<int16_t>(h);
  return true;
}

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // PNG file layout: 8-byte signature, then chunks. The IHDR chunk is mandatory and
  // must be the first chunk: 4 bytes length + "IHDR" + 13 bytes IHDR data + 4 bytes CRC.
  // Width and height live at bytes 16..23 (big-endian uint32s) of the file. Reading
  // those bytes directly avoids allocating any decode buffers.
  FsFile f;
  if (!Storage.openFileForRead("PNG", imagePath, f)) {
    LOG_ERR("PNG", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }

  uint8_t hdr[24];
  int n = f.read(hdr, sizeof(hdr));
  f.close();
  if (n < (int)sizeof(hdr)) {
    LOG_ERR("PNG", "Short read on PNG header: %s", imagePath.c_str());
    return false;
  }

  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(hdr, kPngSig, 8) != 0) {
    LOG_ERR("PNG", "Not a PNG file: %s", imagePath.c_str());
    return false;
  }
  if (hdr[12] != 'I' || hdr[13] != 'H' || hdr[14] != 'D' || hdr[15] != 'R') {
    LOG_ERR("PNG", "First chunk not IHDR: %s", imagePath.c_str());
    return false;
  }

  uint32_t width = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | (uint32_t)hdr[19];
  uint32_t height =
      ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | (uint32_t)hdr[23];
  if (width == 0 || height == 0 || width > 0x7FFF || height > 0x7FFF) {
    LOG_ERR("PNG", "Implausible PNG dimensions %ux%u: %s", width, height, imagePath.c_str());
    return false;
  }

  out.width = (int16_t)width;
  out.height = (int16_t)height;
  return true;
}

adaptive_tone::Points PngToFramebufferConverter::analyzeAdaptiveTone(const std::string& imagePath,
                                                                     const adaptive_tone::Mode mode,
                                                                     const int eqBlendNum) {
  adaptive_tone::Points points;

  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < pngDecodeHeapFloor()) {
    LOG_DBG("PNG", "Skipping adaptive tone analysis, low heap (%u free)", (unsigned)freeHeap);
    return points;
  }

  FsFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) return points;

  auto decoder = std::unique_ptr<PngStreamDecoder>(new (std::nothrow) PngStreamDecoder());
  if (!decoder) {
    file.close();
    return points;
  }
  // This analysis is a FULL second inflate of the image (see the loop below), so it pays the
  // same ring cost as a real decode — route it through the pass scratch too.
  decoder->setScratchArena(image_scratch::get());
  PngStreamDecoder::Info info;
  if (!decoder->begin(file, info)) {
    file.close();
    return points;
  }

  auto histogram = makeUniqueNoThrow<uint32_t[]>(256);
  auto grayLine = makeUniqueNoThrow<uint8_t[]>(info.width);
  if (!histogram || !grayLine) {
    file.close();
    return points;
  }

  // Inflate is sequential, so every row must be decoded even when it is not sampled.
  // ROW_STEP therefore only saves the per-pixel histogram work, not the decode --
  // which is why this whole function costs a full extra decode.
  uint64_t sampled = 0;
  bool ok = true;
  for (uint32_t y = 0; y < info.height; y++) {
    const bool sample = (y % adaptive_tone::ROW_STEP) == 0;
    if (!(sample ? decoder->nextRow(grayLine.get()) : decoder->skipRow())) {
      ok = false;
      break;
    }
    if (!sample) continue;
    for (uint32_t x = 0; x < info.width; x++) histogram[grayLine[x]]++;
    sampled += info.width;
  }

  file.close();
  if (!ok || sampled == 0) return points;

  points = adaptive_tone::derivePoints(histogram.get(), sampled, mode, eqBlendNum);
  if (points.active) {
    LOG_TRC("PNG", "Adaptive tone (%s) enabled: black=%u white=%u",
            mode == adaptive_tone::Mode::Equalize ? "equalize" : "stretch", (unsigned)points.blackPoint,
            (unsigned)points.whitePoint);
  } else {
    LOG_TRC("PNG", "Adaptive tone (%s) declined: line art or range too narrow",
            mode == adaptive_tone::Mode::Equalize ? "equalize" : "stretch");
  }
  return points;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  FsFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) {
    LOG_ERR("PNG", "Failed to open PNG: %s", imagePath.c_str());
    return false;
  }
  const bool ok = decodeOpenFile(file, imagePath, renderer, config);
  file.close();
  return ok;
}

bool PngToFramebufferConverter::decodeOpenFile(FsFile& file, const std::string& label, GfxRenderer& renderer,
                                               const RenderConfig& config) {
  LOG_TRC("PNG", "Decoding PNG: %s", label.c_str());
  const std::string& imagePath = label;  // logging only; the stream is the caller's

  const size_t freeHeap = ESP.getFreeHeap();
  if (const size_t floor = pngDecodeHeapFloor(); freeHeap < floor) {
    LOG_ERR("PNG", "Not enough heap for PNG decode (%u free, need %u)", (unsigned)freeHeap, (unsigned)floor);
    image_scratch::noteHeapShort();
    return false;
  }

  auto decoder = std::unique_ptr<PngStreamDecoder>(new (std::nothrow) PngStreamDecoder());
  if (!decoder) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    image_scratch::noteHeapShort();
    return false;
  }
  decoder->setScratchArena(image_scratch::get());
  PngStreamDecoder::Info info;
  if (!decoder->begin(file, info)) {
    LOG_ERR("PNG", "Failed to start PNG decode: %s", imagePath.c_str());
    if (decoder->outOfMemory()) image_scratch::noteHeapShort();
    return false;
  }

  const int srcWidth = static_cast<int>(info.width);
  const int srcHeight = static_cast<int>(info.height);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Output dimensions (same policy as the old decoder).
  int dstWidth, dstHeight;
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    dstWidth = config.maxWidth;
    dstHeight = config.maxHeight;
  } else {
    const float scaleX = (float)config.maxWidth / srcWidth;
    const float scaleY = (float)config.maxHeight / srcHeight;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;  // never upscale
    dstWidth = (int)(srcWidth * scale);
    dstHeight = (int)(srcHeight * scale);
    if (dstWidth < 1) dstWidth = 1;
    if (dstHeight < 1) dstHeight = 1;
  }
  // No single scale factor: the axes map independently below (Bresenham across, pull-per-row
  // down), so nothing here relies on the caller having handed us an aspect-correct box.
  LOG_TRC("PNG", "PNG %dx%d -> %dx%d (scale %.2fx%.2f), colorType=%d bitDepth=%d", srcWidth, srcHeight, dstWidth,
          dstHeight, (float)dstWidth / srcWidth, (float)dstHeight / srcHeight, info.colorType, info.bitDepth);

  auto grayLine = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[srcWidth]);
  if (!grayLine) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    image_scratch::noteHeapShort();
    return false;
  }

  // Native grayscale short-circuits most of the machinery below: no ditherer, no
  // pixel cache, no companion rendition. All three exist to make 2 bits per pixel
  // carry as much as they can, and this path is not spending 2 bits per pixel.
  const bool gray8Out = config.gray8 != nullptr;

  DitherState dither;
  dither.config = &config;
  if (gray8Out) {
    // no ditherer
  } else if (config.monochromeOutput) {
    dither.atkinson1Bit.reset(new (std::nothrow) Atkinson1BitDitherer(dstWidth));
  }
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  else if (config.useDithering) {
    switch (config.ditherMode) {
      case ImageDitherMode::Atkinson:
        dither.atkinson4.reset(new (std::nothrow) AtkinsonDitherer(dstWidth));
        break;
      case ImageDitherMode::DiffusedBayer:
        dither.bayerDiff.reset(new (std::nothrow) DiffusedBayerDitherer(dstWidth));
        break;
      default:
        break;
    }
  }
#endif

  // Stream the 2-bit pixel cache to disk one row band at a time.
  PixelCache cache;
  bool caching = !config.cachePath.empty() && !gray8Out;
  if (caching && !cache.begin(config.cachePath, dstWidth, dstHeight, config.x, config.y, 1)) {
    LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
    caching = false;
  }

  // Optional second rendition off the same inflate (see CompanionSink). Its failures are
  // independent of the primary's: losing the companion costs a later second decode, which is
  // exactly the status quo, so it must never take the primary cache or the render down with it.
  CompanionSink companion;
  if (!config.companionCachePath.empty() && !gray8Out) {
    if (!config.monochromeOutput) {
      companion.atkinson1Bit.reset(new (std::nothrow) Atkinson1BitDitherer(dstWidth));
    }
    companion.caching = companion.cache.begin(config.companionCachePath, dstWidth, dstHeight, config.x, config.y, 1);
    if (!companion.caching) {
      LOG_ERR("PNG", "Failed to start companion cache stream, continuing with one variant");
    } else {
      LOG_TRC("PNG", "Companion variant streaming to %s", config.companionCachePath.c_str());
    }
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  bool ok = true;
  int decodedSrcY = -1;  // source row currently held in grayLine
  const unsigned long decodeStart = millis();

  // Driven by the DESTINATION rows, pulling the source row each one needs. Deriving dstY from
  // srcY (the obvious direction) leaves output rows unwritten whenever dstHeight differs from
  // srcHeight * scale — one row to integer rounding, or everything below the picture when the
  // caller passes a box of another aspect ratio. An unwritten row is not merely missing: it is
  // filled in by PixelCache and baked into the .pxc, so it is replayed under the image on every
  // later view (and before PixelCache::FILL_BYTE that fill was black). Pulling also makes an
  // upscale replicate rows instead of leaving gaps between them.
  for (int dstY = 0; dstY < dstHeight; dstY++) {
    const int outY = config.y + dstY;
    if (outY >= screenHeight) break;

    const int wantSrcY = (int)((int64_t)dstY * srcHeight / dstHeight);
    while (decodedSrcY < wantSrcY) {
      // Only the row about to be drawn needs its pixels. The ones a downscale collapses still
      // have to be inflated — DEFLATE is sequential — but not converted.
      const bool needPixels = (decodedSrcY + 1 == wantSrcY);
      if (!(needPixels ? decoder->nextRow(grayLine.get()) : decoder->skipRow())) {
        LOG_ERR("PNG", "Decode failed at row %d", decodedSrcY + 1);
        ok = false;
        break;
      }
      decodedSrcY++;
      // Feed the WDT periodically: a large image can take seconds to inflate.
      if ((decodedSrcY & 31) == 0) HalSystem::feedWatchdog();
    }
    if (!ok) break;

    pw.beginRow(outY);
    if (gray8Out) config.gray8->beginRow(outY);

    DirectCacheWriter cw;
    bool rowCaching = caching;
    if (rowCaching) {
      if (!cache.advanceTo(dstY)) {
        caching = false;
        rowCaching = false;
      } else {
        cw.init(cache.buffer, cache.bytesPerRow, cache.originX, config.y + cache.bandStart, cache.width,
                cache.bandRows);
        cw.beginRow(outY);
      }
    }

    companion.rowCaching = companion.caching;
    if (companion.rowCaching) {
      if (!companion.cache.advanceTo(dstY)) {
        companion.caching = false;
        companion.rowCaching = false;
      } else {
        companion.writer.init(companion.cache.buffer, companion.cache.bytesPerRow, companion.cache.originX,
                              config.y + companion.cache.bandStart, companion.cache.width, companion.cache.bandRows);
        companion.writer.beginRow(outY);
      }
    }

    // Bresenham-style horizontal scaling: advance srcX by srcWidth/dstWidth per dst column.
    int srcX = 0;
    int error = 0;
    for (int dstX = 0; dstX < dstWidth; dstX++) {
      const int outX = config.x + dstX;
      // Level-correct once, then hand the SAME sample to both ditherers: the two renditions must
      // differ only in dither, never in the tone they were dithered from. Identity when the
      // caller supplied no points (every reader image; only the sleep screen supplies any).
      const uint8_t gray = adaptive_tone::apply(config.adaptiveTone, grayLine[srcX]);
      if (gray8Out) {
        // The tone-mapped sample IS the output; the panel quantises it later at its
        // own depth. No ditherer runs, so unlike the branch below there is no
        // diffusion state that has to be advanced across off-screen columns.
        if (outX >= 0 && outX < screenWidth) config.gray8->writePixel(outX, gray);
      } else {
        const uint8_t value = ditherGray(dither, gray, dstX, outX, outY);
        // Both ditherers see every destination column, on-screen or not, so their diffusion state
        // stays in step across the row; only the writes below are bounds-guarded.
        const uint8_t companionValue = companion.caching ? companion.dither(gray, dstX, outX, outY) : 0;
        if (outX >= 0 && outX < screenWidth) {
          pw.writePixel(outX, value);
          if (rowCaching) cw.writePixel(outX, value);
          if (companion.rowCaching) companion.writer.writePixel(outX, companionValue);
        }
      }
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
    advanceDitherRow(dither);
    if (companion.caching) companion.nextRow();
  }

  decoder->end();

  if (caching) {
    if (ok) {
      cache.finalize();
    } else {
      cache.abort();
    }
  }
  if (companion.caching) {
    if (ok) {
      companion.cache.finalize();
    } else {
      companion.cache.abort();
    }
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms%s", millis() - decodeStart,
          companion.caching ? " (both variants)" : "");
  return ok;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
