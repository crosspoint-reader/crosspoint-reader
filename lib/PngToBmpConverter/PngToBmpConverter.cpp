#include "PngToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "BmpStreamWriter.h"

// Paeth predictor function per PNG spec
inline uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  int p = static_cast<int>(a) + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

namespace {
// PNG constants
uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// PNG color types
enum PngColorType : uint8_t {
  PNG_COLOR_GRAYSCALE = 0,
  PNG_COLOR_RGB = 2,
  PNG_COLOR_PALETTE = 3,
  PNG_COLOR_GRAYSCALE_ALPHA = 4,
  PNG_COLOR_RGBA = 6,
};

// PNG filter types
enum PngFilter : uint8_t {
  PNG_FILTER_NONE = 0,
  PNG_FILTER_SUB = 1,
  PNG_FILTER_UP = 2,
  PNG_FILTER_AVERAGE = 3,
  PNG_FILTER_PAETH = 4,
};

// Read a big-endian 32-bit value from file
bool readBE32(HalFile& file, uint32_t& value) {
  uint8_t buf[4];
  if (file.read(buf, 4) != 4) return false;
  value = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
          (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
  return true;
}

}  // namespace

// Context for streaming PNG decompression
struct PngDecodeContext {
  InflateStream reader;
  HalFile* file;

  // PNG image properties
  uint32_t width;
  uint32_t height;
  uint8_t bitDepth;
  uint8_t colorType;
  uint8_t bytesPerPixel;  // after expanding sub-byte depths
  uint32_t rawRowBytes;   // bytes per raw row (without filter byte)

  // Scanline buffers
  uint8_t* currentRow;   // current defiltered scanline
  uint8_t* previousRow;  // previous defiltered scanline

  // Chunk reading state
  uint32_t chunkBytesRemaining;  // bytes left in current IDAT chunk
  bool idatFinished;             // no more IDAT chunks

  // File read buffer for feeding the inflate stream
  uint8_t readBuf[2048];

  // Palette for indexed color (type 3)
  uint8_t palette[256 * 3];
  int paletteSize;
};

// Read the next IDAT chunk header, skipping non-IDAT chunks
// Returns true if an IDAT chunk was found
static bool findNextIdatChunk(PngDecodeContext& ctx) {
  while (true) {
    uint32_t chunkLen;
    if (!readBE32(*ctx.file, chunkLen)) return false;

    uint8_t chunkType[4];
    if (ctx.file->read(chunkType, 4) != 4) return false;

    if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      return true;
    }

    // Skip this chunk's data + 4-byte CRC
    // Use seek to skip efficiently
    if (!ctx.file->seekCur(chunkLen + 4)) return false;

    // If we hit IEND, there are no more chunks
    if (memcmp(chunkType, "IEND", 4) == 0) {
      return false;
    }
  }
}

// Fill callback: reads the next batch of IDAT data from the file
static size_t pngIdatFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<PngDecodeContext*>(vctx);

  if (ctx->idatFinished) return 0;

  // Skip 4-byte CRC and find next IDAT chunk when current chunk is exhausted
  while (ctx->chunkBytesRemaining == 0) {
    if (!ctx->file->seekCur(4)) {  // skip 4-byte CRC of previous IDAT
      ctx->idatFinished = true;
      return 0;
    }
    if (!findNextIdatChunk(*ctx)) {
      ctx->idatFinished = true;
      return 0;
    }
  }

  // Read from current IDAT chunk into the read buffer
  size_t toRead = sizeof(ctx->readBuf);
  if (toRead > ctx->chunkBytesRemaining) toRead = ctx->chunkBytesRemaining;

  const int bytesRead = ctx->file->read(ctx->readBuf, toRead);
  if (bytesRead <= 0) {
    ctx->idatFinished = true;
    return 0;
  }

  ctx->chunkBytesRemaining -= bytesRead;
  *data = ctx->readBuf;
  return static_cast<size_t>(bytesRead);
}

// Decode one scanline: decompress filter byte + raw bytes, then unfilter
static bool decodeScanline(PngDecodeContext& ctx) {
  // Decompress filter byte
  uint8_t filterType;
  if (!ctx.reader.read(&filterType, 1)) return false;

  // Decompress raw row data into currentRow
  if (!ctx.reader.read(ctx.currentRow, ctx.rawRowBytes)) return false;

  // Apply reverse filter
  const int bpp = ctx.bytesPerPixel;

  switch (filterType) {
    case PNG_FILTER_NONE:
      break;

    case PNG_FILTER_SUB:
      for (uint32_t i = bpp; i < ctx.rawRowBytes; i++) {
        ctx.currentRow[i] += ctx.currentRow[i - bpp];
      }
      break;

    case PNG_FILTER_UP:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        ctx.currentRow[i] += ctx.previousRow[i];
      }
      break;

    case PNG_FILTER_AVERAGE:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        ctx.currentRow[i] += (a + b) / 2;
      }
      break;

    case PNG_FILTER_PAETH:
      for (uint32_t i = 0; i < ctx.rawRowBytes; i++) {
        uint8_t a = (i >= static_cast<uint32_t>(bpp)) ? ctx.currentRow[i - bpp] : 0;
        uint8_t b = ctx.previousRow[i];
        uint8_t c = (i >= static_cast<uint32_t>(bpp)) ? ctx.previousRow[i - bpp] : 0;
        ctx.currentRow[i] += paethPredictor(a, b, c);
      }
      break;

    default:
      LOG_ERR("PNG", "Unknown filter type: %d", filterType);
      return false;
  }

  return true;
}

// Batch-convert an entire scanline to grayscale.
// Branches once on colorType/bitDepth, then runs a tight loop for the whole row.
static void convertScanlineToGray(const PngDecodeContext& ctx, uint8_t* grayRow) {
  const uint8_t* src = ctx.currentRow;
  const uint32_t w = ctx.width;

  switch (ctx.colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (ctx.bitDepth == 8) {
        memcpy(grayRow, src, w);
      } else if (ctx.bitDepth == 16) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        const int ppb = 8 / ctx.bitDepth;
        const uint8_t mask = (1 << ctx.bitDepth) - 1;
        for (uint32_t x = 0; x < w; x++) {
          int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
          grayRow[x] = (src[x / ppb] >> shift & mask) * 255 / mask;
        }
      }
      break;

    case PNG_COLOR_RGB:
      if (ctx.bitDepth == 8) {
        // Fast path: most common EPUB cover format
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 3;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 6] * 25 + src[x * 6 + 2] * 50 + src[x * 6 + 4] * 25) / 100;
        }
      }
      break;

    case PNG_COLOR_PALETTE: {
      const int ppb = 8 / ctx.bitDepth;
      const uint8_t mask = (1 << ctx.bitDepth) - 1;
      const uint8_t* pal = ctx.palette;
      const int palSize = ctx.paletteSize;
      for (uint32_t x = 0; x < w; x++) {
        int shift = (ppb - 1 - (x % ppb)) * ctx.bitDepth;
        uint8_t idx = (src[x / ppb] >> shift) & mask;
        if (idx >= palSize) idx = 0;
        grayRow[x] = (pal[idx * 3] * 25 + pal[idx * 3 + 1] * 50 + pal[idx * 3 + 2] * 25) / 100;
      }
      break;
    }

    case PNG_COLOR_GRAYSCALE_ALPHA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 4];
      }
      break;

    case PNG_COLOR_RGBA:
      if (ctx.bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 4;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 8] * 25 + src[x * 8 + 2] * 50 + src[x * 8 + 4] * 25) / 100;
        }
      }
      break;

    default:
      memset(grayRow, 128, w);
      break;
  }
}

bool PngToBmpConverter::pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop, bool originalThresholds) {
  LOG_DBG("PNG", "Converting PNG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // Verify PNG signature
  uint8_t sig[8];
  if (pngFile.read(sig, 8) != 8 || memcmp(sig, PNG_SIGNATURE, 8) != 0) {
    LOG_ERR("PNG", "Invalid PNG signature");
    return false;
  }

  // Read IHDR chunk
  uint32_t ihdrLen;
  if (!readBE32(pngFile, ihdrLen)) return false;

  uint8_t ihdrType[4];
  if (pngFile.read(ihdrType, 4) != 4 || memcmp(ihdrType, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Missing IHDR chunk");
    return false;
  }

  uint32_t width, height;
  if (!readBE32(pngFile, width) || !readBE32(pngFile, height)) return false;

  uint8_t ihdrRest[5];
  if (pngFile.read(ihdrRest, 5) != 5) return false;

  uint8_t bitDepth = ihdrRest[0];
  uint8_t colorType = ihdrRest[1];
  uint8_t compression = ihdrRest[2];
  uint8_t filter = ihdrRest[3];
  uint8_t interlace = ihdrRest[4];

  // Skip IHDR CRC
  pngFile.seekCur(4);

  LOG_DBG("PNG", "Image: %ux%u, depth=%u, color=%u, interlace=%u", width, height, bitDepth, colorType, interlace);

  if (compression != 0 || filter != 0) {
    LOG_ERR("PNG", "Unsupported compression/filter method");
    return false;
  }

  if (interlace != 0) {
    LOG_ERR("PNG", "Interlaced PNGs not supported");
    return false;
  }

  // Safety limits
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT || width == 0 || height == 0) {
    LOG_ERR("PNG", "Image too large or zero (%ux%u)", width, height);
    return false;
  }

  // Calculate bytes per pixel and raw row bytes
  uint8_t bytesPerPixel;
  uint32_t rawRowBytes;

  switch (colorType) {
    case PNG_COLOR_GRAYSCALE:
      if (bitDepth == 16) {
        bytesPerPixel = 2;
        rawRowBytes = width * 2;
      } else if (bitDepth == 8) {
        bytesPerPixel = 1;
        rawRowBytes = width;
      } else {
        // Sub-byte: 1, 2, or 4 bits
        bytesPerPixel = 1;
        rawRowBytes = (width * bitDepth + 7) / 8;
      }
      break;
    case PNG_COLOR_RGB:
      bytesPerPixel = (bitDepth == 16) ? 6 : 3;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_PALETTE:
      bytesPerPixel = 1;
      rawRowBytes = (width * bitDepth + 7) / 8;
      break;
    case PNG_COLOR_GRAYSCALE_ALPHA:
      bytesPerPixel = (bitDepth == 16) ? 4 : 2;
      rawRowBytes = width * bytesPerPixel;
      break;
    case PNG_COLOR_RGBA:
      bytesPerPixel = (bitDepth == 16) ? 8 : 4;
      rawRowBytes = width * bytesPerPixel;
      break;
    default:
      LOG_ERR("PNG", "Unsupported color type: %d", colorType);
      return false;
  }

  // Validate raw row bytes won't cause memory issues
  if (rawRowBytes > 16384) {
    LOG_ERR("PNG", "Row too large: %u bytes", rawRowBytes);
    return false;
  }

  // Initialize decode context
  PngDecodeContext ctx = {};
  ctx.file = &pngFile;
  ctx.width = width;
  ctx.height = height;
  ctx.bitDepth = bitDepth;
  ctx.colorType = colorType;
  ctx.bytesPerPixel = bytesPerPixel;
  ctx.rawRowBytes = rawRowBytes;
  ctx.paletteSize = 0;

  const size_t scanlineRowBytes = rawRowBytes;
  auto scanlineRows = makeUniqueNoThrow<uint8_t[]>(scanlineRowBytes * 2);
  if (!scanlineRows) {
    LOG_ERR("PNG", "OOM: scanline buffers (%u bytes each)", rawRowBytes);
    return false;
  }
  ctx.currentRow = scanlineRows.get();
  ctx.previousRow = ctx.currentRow + scanlineRowBytes;

  // Scan for PLTE chunk (palette) and first IDAT chunk
  // We need to read chunks until we find IDAT, collecting PLTE along the way
  bool foundIdat = false;
  while (!foundIdat) {
    uint32_t chunkLen;
    if (!readBE32(pngFile, chunkLen)) break;

    uint8_t chunkType[4];
    if (pngFile.read(chunkType, 4) != 4) break;

    if (memcmp(chunkType, "PLTE", 4) == 0) {
      int entries = chunkLen / 3;
      if (entries > 256) entries = 256;
      ctx.paletteSize = entries;
      size_t palBytes = entries * 3;
      pngFile.read(ctx.palette, palBytes);
      // Skip any remaining palette data
      if (chunkLen > palBytes) pngFile.seekCur(chunkLen - palBytes);
      pngFile.seekCur(4);  // CRC
    } else if (memcmp(chunkType, "IDAT", 4) == 0) {
      ctx.chunkBytesRemaining = chunkLen;
      foundIdat = true;
    } else if (memcmp(chunkType, "IEND", 4) == 0) {
      break;
    } else {
      // Skip unknown chunk
      pngFile.seekCur(chunkLen + 4);
    }
  }

  if (!foundIdat) {
    LOG_ERR("PNG", "No IDAT chunk found");
    return false;
  }

  // Initialize streaming decompressor with 32KB window for back-reference history
  if (!ctx.reader.init(true)) {
    LOG_ERR("PNG", "Failed to init inflate stream");
    return false;
  }
  ctx.reader.setFill(pngIdatFillCallback, &ctx);
  // PNG IDAT data is zlib-wrapped (2-byte header + trailing adler32)
  ctx.reader.setZlibWrapped();

  int outWidth;
  int outHeight;
  if (!calculateBmpOutputSize(width, height, targetWidth, targetHeight, crop, outWidth, outHeight)) return false;
  LOG_DBG("PNG", "Scaling %ux%u -> %dx%d (target %dx%d)", width, height, outWidth, outHeight, targetWidth,
          targetHeight);

  BmpStreamWriter output;
  if (!output.begin(bmpOut, outWidth, outHeight, oneBit, originalThresholds)) return false;

  GrayRowScaler scaler;
  if (!scaler.begin(width, height, output)) return false;

  auto grayRow = makeUniqueNoThrow<uint8_t[]>(width);
  if (!grayRow) {
    LOG_ERR("PNG", "OOM: grayscale row buffer (%u bytes)", width);
    return false;
  }

  bool success = true;

  for (uint32_t y = 0; y < height; y++) {
    if (!decodeScanline(ctx)) {
      LOG_ERR("PNG", "Failed to decode scanline %u", y);
      success = false;
      break;
    }

    convertScanlineToGray(ctx, grayRow.get());
    if (!scaler.writeRow(grayRow.get(), y)) {
      success = false;
      break;
    }

    // Swap current/previous row buffers
    uint8_t* temp = ctx.previousRow;
    ctx.previousRow = ctx.currentRow;
    ctx.currentRow = temp;
  }

  if (success) {
    LOG_DBG("PNG", "Successfully converted PNG to BMP");
  }
  return success;
}

bool PngToBmpConverter::pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop, bool originalThresholds) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetWidth, targetHeight, false, crop, originalThresholds);
}

bool PngToBmpConverter::pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
