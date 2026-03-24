#include "PngToBmpConverter.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Same as JpegToBmpConverter for consistency
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr bool USE_PRESCALE = true;
constexpr int TARGET_MAX_WIDTH = 480;
constexpr int TARGET_MAX_HEIGHT = 800;
// ============================================================================

// BMP writing helpers (same as JpegToBmpConverter)
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
bool readBE32(FsFile& file, uint32_t& value) {
  uint8_t buf[4];
  if (file.read(buf, 4) != 4) return false;
  value = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
          (static_cast<uint32_t>(buf[2]) << 8) | buf[3];
  return true;
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
  for (const uint8_t i : palette) {
    bmpOut.write(i);
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
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}
}  // namespace

// Context for streaming PNG decompression
// IMPORTANT: reader must be the first field - the uzlib callback casts uzlib_uncomp* to PngDecodeContext*
struct PngDecodeContext {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to PngDecodeContext*
  FsFile* file;

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

  // File read buffer for feeding uzlib
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

// uzlib callback: reads the next batch of IDAT data from the file
static int pngIdatReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<PngDecodeContext*>(uncomp);

  if (ctx->idatFinished) return -1;

  // Skip 4-byte CRC and find next IDAT chunk when current chunk is exhausted
  while (ctx->chunkBytesRemaining == 0) {
    if (!ctx->file->seekCur(4)) {  // skip 4-byte CRC of previous IDAT
      ctx->idatFinished = true;
      return -1;
    }
    if (!findNextIdatChunk(*ctx)) {
      ctx->idatFinished = true;
      return -1;
    }
  }

  // Read from current IDAT chunk into the read buffer
  size_t toRead = sizeof(ctx->readBuf);
  if (toRead > ctx->chunkBytesRemaining) toRead = ctx->chunkBytesRemaining;

  int bytesRead = ctx->file->read(ctx->readBuf, toRead);
  if (bytesRead <= 0) {
    ctx->idatFinished = true;
    return -1;
  }

  ctx->chunkBytesRemaining -= bytesRead;

  // Give uzlib the buffer (skip first byte since we return it directly)
  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
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

bool PngToBmpConverter::pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop) {
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

  // Allocate scanline buffers
  ctx.currentRow = static_cast<uint8_t*>(malloc(rawRowBytes));
  ctx.previousRow = static_cast<uint8_t*>(calloc(rawRowBytes, 1));
  if (!ctx.currentRow || !ctx.previousRow) {
    LOG_ERR("PNG", "Failed to allocate scanline buffers (%u bytes each)", rawRowBytes);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

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
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Initialize streaming decompressor with 32KB ring buffer for back-reference history
  if (!ctx.reader.init(true)) {
    LOG_ERR("PNG", "Failed to init inflate reader");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }
  ctx.reader.setReadCallback(pngIdatReadCallback);
  // PNG IDAT data is zlib-wrapped: consume the 2-byte zlib header (CMF + FLG)
  ctx.reader.skipZlibHeader();

  // Calculate output dimensions (same logic as JpegToBmpConverter)
  int outWidth = width;
  int outHeight = height;
  uint32_t scaleX_fp = 65536;
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 &&
      (static_cast<int>(width) != targetWidth || static_cast<int>(height) != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / width;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / height;
    float scale = 1.0;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(width * scale);
    outHeight = static_cast<int>(height * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (width << 16) / outWidth;
    scaleY_fp = (height << 16) / outHeight;
    needsScaling = true;

    LOG_DBG("PNG", "Scaling %ux%u -> %dx%d (target %dx%d)", width, height, outWidth, outHeight, targetWidth,
            targetHeight);
  }

  // Write BMP header
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  // Allocate BMP row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR("PNG", "Failed to allocate row buffer");
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  // Create ditherers (same as JpegToBmpConverter)
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;

  if (oneBit) {
    atkinson1BitDitherer = new Atkinson1BitDitherer(outWidth);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = new FloydSteinbergDitherer(outWidth);
    }
  }

  // Scaling accumulators
  uint32_t* rowAccum = nullptr;
  uint16_t* rowCount = nullptr;
  int currentOutY = 0;
  uint32_t nextOutY_srcStart = 0;

  if (needsScaling) {
    rowAccum = new uint32_t[outWidth]();
    rowCount = new uint16_t[outWidth]();
    nextOutY_srcStart = scaleY_fp;
  }

  // Allocate grayscale row buffer - batch-convert each scanline to avoid
  // per-pixel getPixelGray() switch overhead in the hot loops
  auto* grayRow = static_cast<uint8_t*>(malloc(width));
  if (!grayRow) {
    LOG_ERR("PNG", "Failed to allocate grayscale row buffer");
    delete[] rowAccum;
    delete[] rowCount;
    delete atkinsonDitherer;
    delete fsDitherer;
    delete atkinson1BitDitherer;
    free(rowBuffer);
    free(ctx.currentRow);
    free(ctx.previousRow);
    return false;
  }

  bool success = true;

  // Process each scanline
  for (uint32_t y = 0; y < height; y++) {
    // Decode one scanline
    if (!decodeScanline(ctx)) {
      LOG_ERR("PNG", "Failed to decode scanline %u", y);
      success = false;
      break;
    }

    // Batch-convert entire scanline to grayscale (one branch, tight loop)
    convertScanlineToGray(ctx, grayRow);

    if (!needsScaling) {
      // Direct output (no scaling)
      memset(rowBuffer, 0, bytesPerRow);

      if (USE_8BIT_OUTPUT && !oneBit) {
        for (int x = 0; x < outWidth; x++) {
          rowBuffer[x] = adjustPixel(grayRow[x]);
        }
      } else if (oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t bit =
              atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(grayRow[x], x) : quantize1bit(grayRow[x], x, y);
          const int byteIndex = x / 8;
          const int bitOffset = 7 - (x % 8);
          rowBuffer[byteIndex] |= (bit << bitOffset);
        }
        if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
      } else {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t gray = adjustPixel(grayRow[x]);
          uint8_t twoBit;
          if (atkinsonDitherer) {
            twoBit = atkinsonDitherer->processPixel(gray, x);
          } else if (fsDitherer) {
            twoBit = fsDitherer->processPixel(gray, x);
          } else {
            twoBit = quantize(gray, x, y);
          }
          const int byteIndex = (x * 2) / 8;
          const int bitOffset = 6 - ((x * 2) % 8);
          rowBuffer[byteIndex] |= (twoBit << bitOffset);
        }
        if (atkinsonDitherer)
          atkinsonDitherer->nextRow();
        else if (fsDitherer)
          fsDitherer->nextRow();
      }
      bmpOut.write(rowBuffer, bytesPerRow);
    } else {
      // Area-averaging scaling (same as JpegToBmpConverter)
      for (int outX = 0; outX < outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < static_cast<int>(width); srcX++) {
          sum += grayRow[srcX];
          count++;
        }

        if (count == 0 && srcXStart < static_cast<int>(width)) {
          sum = grayRow[srcXStart];
          count = 1;
        }

        rowAccum[outX] += sum;
        rowCount[outX] += count;
      }

      // Check if we've crossed into the next output row(s)
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

      // Output all rows whose boundaries we've crossed (handles both up and downscaling)
      // For upscaling, one source row may produce multiple output rows
      while (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, currentOutY);
            const int byteIndex = x / 8;
            const int bitOffset = 7 - (x % 8);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, currentOutY);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }

        bmpOut.write(rowBuffer, bytesPerRow);
        currentOutY++;

        nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;

        // For upscaling: don't reset accumulators if next output row uses same source data
        // Only reset when we'll move to a new source row
        if (srcY_fp >= nextOutY_srcStart) {
          // More output rows to emit from same source - keep accumulator data
          continue;
        }
        // Moving to next source row - reset accumulators
        memset(rowAccum, 0, outWidth * sizeof(uint32_t));
        memset(rowCount, 0, outWidth * sizeof(uint16_t));
      }
    }

    // Swap current/previous row buffers
    uint8_t* temp = ctx.previousRow;
    ctx.previousRow = ctx.currentRow;
    ctx.currentRow = temp;
  }

  // Clean up
  free(grayRow);
  delete[] rowAccum;
  delete[] rowCount;
  delete atkinsonDitherer;
  delete fsDitherer;
  delete atkinson1BitDitherer;
  free(rowBuffer);
  free(ctx.currentRow);
  free(ctx.previousRow);

  if (success) {
    LOG_DBG("PNG", "Successfully converted PNG to BMP");
  }
  return success;
}

bool PngToBmpConverter::pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, TARGET_MAX_WIDTH, TARGET_MAX_HEIGHT, false, crop);
}

bool PngToBmpConverter::pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
