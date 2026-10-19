#include "GifToFramebufferConverter.h"

#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>

#include <cstring>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"  // also image_scratch::noteHeapShort

namespace {

static constexpr int GIF_MAX_CODES = 4096;
static constexpr uint16_t GIF_NO_PREFIX = 0xFFFF;

// LZW string table + I/O buffers, heap-allocated (~18 KB).
struct GifState {
  uint16_t prefix[GIF_MAX_CODES];   // 8 KB
  uint8_t suffix[GIF_MAX_CODES];    // 4 KB
  uint8_t lzwStack[GIF_MAX_CODES];  // 4 KB output stack

  uint8_t globalColorTable[256 * 3];
  uint8_t localColorTable[256 * 3];

  // Active color table (points to global or local)
  uint8_t* colorTable;
  int colorTableSize;

  // Screen / frame layout
  int screenWidth, screenHeight;
  int frameLeft, frameTop;
  int frameWidth, frameHeight;
  bool interlaced;

  // Transparency (Graphic Control Extension)
  bool hasTransparent;
  uint8_t transparentIndex;

  // LZW decode state
  int minCodeSize, clearCode, eoiCode, nextCode, codeSize;
  uint32_t bitBuffer;
  int bitsInBuffer;

  // Sub-block streaming
  uint8_t blockBuf[256];
  int blockPos, blockLen;
};

static bool readColorTable(FsFile& f, uint8_t* table, int numColors) {
  return f.read(table, numColors * 3) == numColors * 3;
}

static bool skipSubBlocks(FsFile& f) {
  for (;;) {
    uint8_t len;
    if (f.read(&len, 1) != 1) return false;
    if (len == 0) return true;
    if (!f.seek(f.position() + len)) return false;
  }
}

static void gifInitLzw(GifState& st, int minCodeSize) {
  st.minCodeSize = minCodeSize;
  st.clearCode = 1 << minCodeSize;
  st.eoiCode = st.clearCode + 1;
  st.nextCode = st.eoiCode + 1;
  st.codeSize = minCodeSize + 1;
  st.bitBuffer = 0;
  st.bitsInBuffer = 0;
  st.blockPos = 0;
  st.blockLen = 0;
  for (int i = 0; i < st.clearCode; i++) {
    st.suffix[i] = (uint8_t)i;
    st.prefix[i] = GIF_NO_PREFIX;
  }
}

// Returns byte value or -1 on block-stream end / error.
static int gifReadByte(FsFile& f, GifState& st) {
  while (st.blockPos >= st.blockLen) {
    uint8_t len;
    if (f.read(&len, 1) != 1 || len == 0) return -1;
    if ((int)f.read(st.blockBuf, len) != (int)len) return -1;
    st.blockPos = 0;
    st.blockLen = (int)len;
  }
  return st.blockBuf[st.blockPos++];
}

static int gifReadCode(FsFile& f, GifState& st) {
  while (st.bitsInBuffer < st.codeSize) {
    int b = gifReadByte(f, st);
    if (b < 0) return -1;
    st.bitBuffer |= ((uint32_t)(uint8_t)b << st.bitsInBuffer);
    st.bitsInBuffer += 8;
  }
  int code = (int)(st.bitBuffer & ((1u << st.codeSize) - 1u));
  st.bitBuffer >>= st.codeSize;
  st.bitsInBuffer -= st.codeSize;
  return code;
}

static void indexedToGray(const uint8_t* src, uint8_t* gray, int width, const GifState& st) {
  const uint8_t* ct = st.colorTable;
  const int ctSize = st.colorTableSize;
  for (int x = 0; x < width; x++) {
    uint8_t idx = src[x];
    if (st.hasTransparent && idx == st.transparentIndex) {
      gray[x] = 255;
    } else if (idx < ctSize) {
      const uint8_t* p = &ct[idx * 3];
      gray[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    } else {
      gray[x] = 0;
    }
  }
}

// Parse GIF header, LSD, optional GCT, and extensions up to (and including) the
// first Image Descriptor. Leaves `f` positioned at the LZW minimum code size byte.
// Returns false if the file is truncated or no image was found.
static bool parseGifHeader(FsFile& f, GifState& st) {
  uint8_t hdr[6];
  if (f.read(hdr, 6) != 6 || hdr[0] != 'G' || hdr[1] != 'I' || hdr[2] != 'F') return false;

  uint8_t lsd[7];
  if (f.read(lsd, 7) != 7) return false;
  bool hasGct = (lsd[4] >> 7) & 1;
  int gctColors = hasGct ? (2 << (lsd[4] & 0x07)) : 0;
  st.hasTransparent = false;
  st.transparentIndex = 0;
  st.colorTable = nullptr;
  st.colorTableSize = 0;

  if (hasGct) {
    if (!readColorTable(f, st.globalColorTable, gctColors)) return false;
    st.colorTable = st.globalColorTable;
    st.colorTableSize = gctColors;
  }

  for (;;) {
    uint8_t blockId;
    if (f.read(&blockId, 1) != 1) return false;
    if (blockId == 0x3B) return false;  // Trailer with no image

    if (blockId == 0x21) {  // Extension
      uint8_t label;
      if (f.read(&label, 1) != 1) return false;
      if (label == 0xF9) {
        uint8_t subLen;
        if (f.read(&subLen, 1) != 1) return false;
        uint8_t data[4] = {};
        int toRead = (subLen < 4) ? (int)subLen : 4;
        if (toRead > 0 && f.read(data, toRead) != toRead) return false;
        if (subLen > toRead && !f.seek(f.position() + (subLen - toRead))) return false;
        if (data[0] & 0x01) {
          st.hasTransparent = true;
          st.transparentIndex = data[3];
        }
      }
      skipSubBlocks(f);
      continue;
    }

    if (blockId == 0x2C) {  // Image Descriptor
      uint8_t desc[9];
      if (f.read(desc, 9) != 9) return false;
      st.frameLeft = desc[0] | (desc[1] << 8);
      st.frameTop = desc[2] | (desc[3] << 8);
      st.frameWidth = desc[4] | (desc[5] << 8);
      st.frameHeight = desc[6] | (desc[7] << 8);
      st.interlaced = (desc[8] >> 6) & 1;
      bool hasLct = (desc[8] >> 7) & 1;
      int lctColors = hasLct ? (2 << (desc[8] & 0x07)) : 0;
      if (hasLct) {
        if (!readColorTable(f, st.localColorTable, lctColors)) return false;
        st.colorTable = st.localColorTable;
        st.colorTableSize = lctColors;
      }
      return st.frameWidth > 0 && st.frameHeight > 0 && st.colorTable != nullptr;
    }

    return false;  // Unknown block type
  }
}

// Run the LZW decode loop, calling `onRow(actualY, indexedRow, width)` for each
// completed source row. Returns the number of rows successfully decoded.
// Interlace pass accounting is handled here; `onRow` receives the display-order Y.
template <typename RowSink>
static int gifDecodeLzw(FsFile& f, GifState& st, uint8_t* indexedRow, RowSink onRow) {
  static const int kPassStart[] = {0, 4, 2, 1};
  static const int kPassStep[] = {8, 8, 4, 2};

  int pixelX = 0, srcRow = 0, pass = 0;
  int actualY = st.interlaced ? kPassStart[0] : 0;
  bool firstCode = true;
  int oldCode = -1;

  while (srcRow < st.frameHeight) {
    int code = gifReadCode(f, st);
    if (code < 0) break;

    if (code == st.clearCode) {
      st.nextCode = st.eoiCode + 1;
      st.codeSize = st.minCodeSize + 1;
      firstCode = true;
      oldCode = -1;
      continue;
    }
    if (code == st.eoiCode) break;

    int stackTop = 0;
    int c = code;

    if (!firstCode && code == st.nextCode) {
      // KwKwK: output is S(oldCode) + first_char(oldCode). Reserve index 0 for
      // first_char (emitted last), build the chain above it, then fill index 0
      // with the root character discovered during the walk — no memmove needed.
      stackTop = 1;  // reserve slot 0
      c = oldCode;
      while (st.prefix[c] != GIF_NO_PREFIX && stackTop < GIF_MAX_CODES - 1) {
        st.lzwStack[stackTop++] = st.suffix[c];
        c = st.prefix[c];
      }
      st.lzwStack[stackTop++] = st.suffix[c];  // root at top, emitted first
      st.lzwStack[0] = st.suffix[c];           // fill reserved slot: first_char emitted last
    } else if (code <= st.eoiCode || code < st.nextCode) {
      while (st.prefix[c] != GIF_NO_PREFIX && stackTop < GIF_MAX_CODES - 1) {
        st.lzwStack[stackTop++] = st.suffix[c];
        c = st.prefix[c];
      }
      st.lzwStack[stackTop++] = st.suffix[c];
    } else {
      break;
    }

    uint8_t firstChar = st.lzwStack[stackTop - 1];

    if (!firstCode && st.nextCode < GIF_MAX_CODES) {
      st.prefix[st.nextCode] = (uint16_t)oldCode;
      st.suffix[st.nextCode] = firstChar;
      st.nextCode++;
      if (st.nextCode >= (1 << st.codeSize) && st.codeSize < 12) st.codeSize++;
    }
    firstCode = false;
    oldCode = code;

    for (int i = stackTop - 1; i >= 0 && srcRow < st.frameHeight; i--) {
      indexedRow[pixelX++] = st.lzwStack[i];
      if (pixelX >= st.frameWidth) {
        if (!onRow(actualY, indexedRow, st.frameWidth)) return -1;
        pixelX = 0;
        srcRow++;
        if (st.interlaced) {
          actualY += kPassStep[pass];
          while (actualY >= st.frameHeight && pass < 3) actualY = kPassStart[++pass];
        } else {
          actualY++;
        }
      }
    }
  }

  return srcRow;
}

}  // namespace

bool GifToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out) {
  if (!buf || len < 10) return false;
  if (buf[0] != 'G' || buf[1] != 'I' || buf[2] != 'F') return false;
  uint16_t w = (uint16_t)(buf[6] | (buf[7] << 8));
  uint16_t h = (uint16_t)(buf[8] | (buf[9] << 8));
  if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
  out.width = (int16_t)w;
  out.height = (int16_t)h;
  return true;
}

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  FsFile f;
  if (!Storage.openFileForRead("GIF", imagePath, f)) {
    LOG_ERR("GIF", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }
  uint8_t hdr[10];
  int n = f.read(hdr, sizeof(hdr));
  f.close();
  return n >= (int)sizeof(hdr) && getDimensionsFromBuffer(hdr, sizeof(hdr), out);
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasGifExtension(extension);
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("GIF", "Decoding GIF: %s", imagePath.c_str());

  GifState* st = new (std::nothrow) GifState();
  if (!st) {
    LOG_ERR("GIF", "Failed to allocate GIF state");
    image_scratch::noteHeapShort();
    return false;
  }

  FsFile f;
  auto cleanup = [&]() {
    f.close();
    delete st;
  };

  if (!Storage.openFileForRead("GIF", imagePath, f) || !parseGifHeader(f, *st)) {
    LOG_ERR("GIF", "Failed to parse GIF: %s", imagePath.c_str());
    cleanup();
    return false;
  }

  if (!validateImageDimensions(st->frameWidth, st->frameHeight, "GIF")) {
    cleanup();
    return false;
  }

  const int srcW = st->frameWidth;
  const int srcH = st->frameHeight;
  float scale;
  int dstW, dstH;
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    dstW = config.maxWidth;
    dstH = config.maxHeight;
    scale = (float)dstW / srcW;
  } else {
    float sx = (float)config.maxWidth / srcW;
    float sy = (float)config.maxHeight / srcH;
    scale = (sx < sy) ? sx : sy;
    if (scale > 1.0f) scale = 1.0f;
    dstW = (int)(srcW * scale);
    dstH = (int)(srcH * scale);
  }
  if (dstW <= 0 || dstH <= 0) {
    LOG_ERR("GIF", "Zero-sized output for: %s", imagePath.c_str());
    cleanup();
    return false;
  }

  uint8_t lzwMin;
  if (f.read(&lzwMin, 1) != 1 || lzwMin < 2 || lzwMin > 11) {
    LOG_ERR("GIF", "Invalid LZW min code size in: %s", imagePath.c_str());
    cleanup();
    return false;
  }
  gifInitLzw(*st, lzwMin);

  LOG_DBG("GIF", "GIF %dx%d -> %dx%d%s", srcW, srcH, dstW, dstH, st->interlaced ? " [interlaced]" : "");

  uint8_t* indexedRow = (uint8_t*)malloc(srcW);
  uint8_t* grayRow = (uint8_t*)malloc(srcW);
  if (!indexedRow || !grayRow) {
    LOG_ERR("GIF", "Failed to alloc row buffers");
    free(indexedRow);
    free(grayRow);
    cleanup();
    return false;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int cfgX = config.x + (int)(st->frameLeft * scale);
  const int cfgY = config.y + (int)(st->frameTop * scale);
  int nextDstY = 0;  // first destination row not yet emitted (progressive GIFs only)
  bool aborted = false;

  DirectPixelWriter pw;
  pw.init(renderer);

  PixelCache cache;
  bool caching = !config.cachePath.empty() && !st->interlaced;
  if (caching && !cache.begin(config.cachePath, dstW, dstH, cfgX, cfgY, 1)) {
    LOG_ERR("GIF", "Failed to start cache stream, continuing without caching");
    caching = false;
  }

  // Paint one destination row from whatever is currently in grayRow.
  const auto emitRow = [&](const int dstY) {
    const int outY = cfgY + dstY;
    if (outY < 0 || outY >= screenH) return;
    pw.beginRow(outY);

    DirectCacheWriter cw;
    bool rowCaching = caching;
    if (rowCaching) {
      if (!cache.advanceTo(dstY)) {
        caching = false;
        rowCaching = false;
      } else {
        cw.init(cache.buffer, cache.bytesPerRow, cache.originX, cfgY + cache.bandStart, cache.width, cache.bandRows);
        cw.beginRow(outY);
      }
    }
    int srcX = 0, err = 0;
    for (int dstX = 0; dstX < dstW; dstX++) {
      int outX = cfgX + dstX;
      if (outX >= 0 && outX < screenW) {
        const uint8_t value = applyBayerDither4Level(grayRow[srcX], outX, outY);
        pw.writePixel(outX, value);
        if (rowCaching) cw.writePixel(outX, value);
      }
      err += srcW;
      while (err >= dstW) {
        err -= dstW;
        srcX++;
      }
    }
  };

  int srcRow = gifDecodeLzw(f, *st, indexedRow, [&](int actualY, const uint8_t* idxRow, int w) {
    if (CooperativeAbort::shouldAbortLongTask()) {
      CooperativeAbort::markAborted();
      aborted = true;
      return false;
    }
    HalSystem::feedWatchdog();
    indexedToGray(idxRow, grayRow, w, *st);

    // Interlaced rows arrive out of order, so there is no monotonic cursor to run and no way
    // to know which neighbours are still coming: map each source row to the single destination
    // row it lands on. Caching is off in this mode (see `caching` above), so a row a downscale
    // never selects is only missing from the screen, not baked into a .pxc.
    if (st->interlaced) {
      const int dstY = (int)((int64_t)actualY * dstH / srcH);
      if (dstY < dstH) emitRow(dstY);
      return true;
    }

    // Progressive: cover EVERY destination row this source row spans, rather than the one row
    // `actualY * scale` happens to land on. That mapping was derived from the horizontal scale,
    // so a box whose aspect ratio does not match the source stopped short and left the rows
    // below the picture to PixelCache's fill; it also skipped rows outright on an upscale.
    int dstYEnd = (int)((int64_t)(actualY + 1) * dstH / srcH);
    if (dstYEnd > dstH) dstYEnd = dstH;
    for (int dstY = nextDstY; dstY < dstYEnd; dstY++) emitRow(dstY);
    if (dstYEnd > nextDstY) nextDstY = dstYEnd;
    return true;
  });

  if (caching) {
    if (srcRow > 0 && !aborted) {
      cache.finalize();
    } else {
      cache.abort();
    }
  }

  free(indexedRow);
  free(grayRow);
  cleanup();
  LOG_DBG("GIF", "GIF decoding complete (%d rows)", srcRow);
  return srcRow > 0 && !aborted;
}

bool GifToFramebufferConverter::decodeFirstFrameToGrayscale(const std::string& path, std::vector<uint8_t>& outPixels,
                                                            ImageDimensions& outDims) {
  GifState* st = new (std::nothrow) GifState();
  if (!st) return false;

  FsFile f;
  auto cleanup = [&]() {
    f.close();
    delete st;
  };

  if (!Storage.openFileForRead("GIF", path, f) || !parseGifHeader(f, *st)) {
    cleanup();
    return false;
  }

  uint8_t lzwMin;
  if (f.read(&lzwMin, 1) != 1 || lzwMin < 2 || lzwMin > 11) {
    cleanup();
    return false;
  }
  gifInitLzw(*st, lzwMin);

  const int w = st->frameWidth;
  const int h = st->frameHeight;
  outDims = {(int16_t)w, (int16_t)h};
  outPixels.assign(w * h, 0);

  uint8_t* indexedRow = (uint8_t*)malloc(w);
  uint8_t* grayRow = (uint8_t*)malloc(w);
  if (!indexedRow || !grayRow) {
    free(indexedRow);
    free(grayRow);
    cleanup();
    return false;
  }

  int srcRow = gifDecodeLzw(f, *st, indexedRow, [&](int actualY, const uint8_t* idxRow, int rowW) {
    indexedToGray(idxRow, grayRow, rowW, *st);
    if (actualY >= 0 && actualY < h) {
      memcpy(&outPixels[actualY * w], grayRow, w);
    }
    return true;
  });

  free(indexedRow);
  free(grayRow);
  cleanup();
  return srcRow > 0;
}
