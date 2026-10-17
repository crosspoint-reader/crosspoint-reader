/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <BufferedPrint.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

bool Xtc::load() {
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());

  // Initialize parser
  parser.reset(new xtc::XtcParser());

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  clearPageCache();  // drop any transposed page planes from a previous session
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("XTC", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  LOG_DBG("XTC", "Cache cleared successfully");
  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

const std::vector<xtc::ChapterInfo>& Xtc::getChapters() {
  static const std::vector<xtc::ChapterInfo> kEmpty;
  if (!loaded || !parser) {
    return kEmpty;
  }
  return parser->getChapters();
}

namespace {

// Extract pixel value (0..3) at column x from a packed 2-bit row (MSB-first).
inline uint8_t packedPixel(const uint8_t* packedRow, int x) {
  const int shift = 6 - (x % 4) * 2;
  return static_cast<uint8_t>((packedRow[x / 4] >> shift) & 0x03);
}

// XTH (2-bit) page data is stored column-major (one column's vertical bytes,
// then the next), but every consumer here walks the page row-major. Holding the
// whole ~94KB page in heap to bridge that transpose is exactly the contiguous
// allocation that fails on the fragmented ESP32-C3 heap during cover/thumb
// generation. Instead we transpose the page once into a row-major temp file of
// 2-bit packed pixels (4px/byte, MSB-first), using a small RAM band, then the
// cover/thumb writers stream that temp one row at a time.
//
// Memory: the band holds BAND_ROWS output rows of packed 2-bit pixels plus a
// chunk of source columns, both bounded and tiny (~a few KB), versus the full
// page buffer it replaces.
struct XthRowTranspose {
  static constexpr int BAND_ROWS = 64;  // rows of packed 2-bit pixels held in RAM

  uint16_t width = 0;
  uint16_t height = 0;
  int packedRowBytes = 0;  // (width + 3) / 4
  uint8_t* band = nullptr;
  uint8_t* column = nullptr;  // chunk of source columns from each plane (plane1|plane2)
  size_t colBytes = 0;
  size_t planeSize = 0;
  FsFile temp;
  std::string tempPath;
  bool ok = false;

  XthRowTranspose(const XthRowTranspose&) = delete;
  XthRowTranspose& operator=(const XthRowTranspose&) = delete;

  // Build the row-major temp from the column-major page. parser is borrowed.
  XthRowTranspose(xtc::XtcParser* parser, uint32_t pageIndex, uint16_t w, uint16_t h, const std::string& path)
      : width(w), height(h), tempPath(path) {
    packedRowBytes = (width + 3) / 4;
    colBytes = (height + 7) / 8;
    planeSize = (static_cast<size_t>(width) * height + 7) / 8;

    // Read this many source columns per file read so the strided transpose does
    // a few hundred large reads instead of ~width*bands tiny ones. Each chunk is
    // COLS_PER_READ*colBytes contiguous bytes per plane (height up to 800 ->
    // colBytes 100, so 32 cols = ~3.2KB/plane).
    static constexpr int COLS_PER_READ = 32;

    // Bounded scratch: BAND_ROWS packed rows + a chunk of source columns from
    // each plane (plane1 chunk | plane2 chunk).
    const size_t bandBytes = static_cast<size_t>(BAND_ROWS) * packedRowBytes;
    const size_t chunkBytes = static_cast<size_t>(COLS_PER_READ) * colBytes;
    band = static_cast<uint8_t*>(malloc(bandBytes));         // freed in dtor
    column = static_cast<uint8_t*>(malloc(chunkBytes * 2));  // plane1|plane2 chunk; freed in dtor
    if (!band || !column) {
      LOG_ERR("XTC", "OOM XTH transpose scratch (%u + %u bytes)", (unsigned)bandBytes, (unsigned)(chunkBytes * 2));
      return;
    }
    if (!Storage.openFileForWrite("XTC", tempPath, temp)) {
      LOG_ERR("XTC", "Failed to open XTH transpose temp: %s", tempPath.c_str());
      return;
    }

    // Walk output rows in bands; within a band read whole source columns in
    // chunks and scatter their 8-pixel groups into the packed band rows.
    for (int bandY = 0; bandY < height; bandY += BAND_ROWS) {
      const int rowsInBand = (height - bandY < BAND_ROWS) ? (height - bandY) : BAND_ROWS;
      memset(band, 0, static_cast<size_t>(rowsInBand) * packedRowBytes);

      for (int colBase = 0; colBase < width; colBase += COLS_PER_READ) {
        const int colsInChunk = (width - colBase < COLS_PER_READ) ? (width - colBase) : COLS_PER_READ;
        const size_t readBytes = static_cast<size_t>(colsInChunk) * colBytes;
        // colBase counts source columns in file order (0..width-1). Read the
        // chunk from both planes in one read each.
        if (parser->loadPageRange(pageIndex, static_cast<size_t>(colBase) * colBytes, column, readBytes) != readBytes ||
            parser->loadPageRange(pageIndex, planeSize + static_cast<size_t>(colBase) * colBytes, column + readBytes,
                                  readBytes) != readBytes) {
          LOG_ERR("XTC", "XTH transpose read error at colBase %d band %d", colBase, bandY);
          parser->endPageRange();
          abortTemp();
          return;
        }

        for (int c = 0; c < colsInChunk; c++) {
          const size_t colIndex = colBase + c;
          // Right-to-left mapping: file column colIndex -> output x.
          const size_t x = width - 1 - colIndex;
          const uint8_t* col1 = column + static_cast<size_t>(c) * colBytes;
          const uint8_t* col2 = column + readBytes + static_cast<size_t>(c) * colBytes;
          const int shift = 6 - (x % 4) * 2;  // MSB-first, pixel 0 at bits 6-7
          for (int r = 0; r < rowsInBand; r++) {
            const int y = bandY + r;
            const size_t byteInCol = y / 8;
            const size_t bitInByte = 7 - (y % 8);
            const uint8_t b1 = (col1[byteInCol] >> bitInByte) & 1;
            const uint8_t b2 = (col2[byteInCol] >> bitInByte) & 1;
            const uint8_t value = static_cast<uint8_t>((b1 << 1) | b2);  // 0..3
            uint8_t* row = band + static_cast<size_t>(r) * packedRowBytes;
            row[x / 4] |= static_cast<uint8_t>((value & 0x03) << shift);
          }
        }
      }

      const size_t toWrite = static_cast<size_t>(rowsInBand) * packedRowBytes;
      if (temp.write(band, toWrite) != toWrite) {
        LOG_ERR("XTC", "XTH transpose write error at band %d", bandY);
        parser->endPageRange();
        abortTemp();
        return;
      }
    }

    parser->endPageRange();
    temp.close();
    // Reopen for sequential row reads.
    if (!Storage.openFileForRead("XTC", tempPath, temp)) {
      LOG_ERR("XTC", "Failed to reopen XTH transpose temp: %s", tempPath.c_str());
      return;
    }
    ok = true;
  }

  // Read one row of packed 2-bit pixels (packedRowBytes) into out.
  bool readRow(int y, uint8_t* out) {
    if (!ok) return false;
    if (!temp.seek64(static_cast<uint64_t>(y) * packedRowBytes)) return false;
    return temp.read(out, packedRowBytes) == packedRowBytes;
  }

  // Cached single-row pixel accessor for downscaling, which sweeps src columns
  // within one src row before moving on. Returns pixel value 0..3, or 0 on error.
  uint8_t* cacheRow = nullptr;  // lazily allocated packed row cache
  int cachedY = -1;
  uint8_t pixelValue(int x, int y) {
    if (!ok) return 0;
    if (y != cachedY) {
      if (!cacheRow) {
        cacheRow = static_cast<uint8_t*>(malloc(packedRowBytes));  // freed in dtor
        if (!cacheRow) return 0;
      }
      if (!readRow(y, cacheRow)) return 0;
      cachedY = y;
    }
    return packedPixel(cacheRow, x);
  }

  void abortTemp() {
    if (temp.isOpen()) temp.close();
    Storage.remove(tempPath.c_str());
  }

  ~XthRowTranspose() {
    if (temp.isOpen()) temp.close();
    if (!tempPath.empty()) Storage.remove(tempPath.c_str());
    if (band) free(band);
    if (column) free(column);
    if (cacheRow) free(cacheRow);
  }
};

}  // namespace

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::coverBmpReady() const { return Storage.exists(getCoverBmpPath().c_str()); }

bool Xtc::generateCoverBmp() const {
  // Already generated
  if (coverBmpReady()) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();
  xtc::XtcParser* mutParser = const_cast<xtc::XtcParser*>(parser.get());

  // Create BMP file
  FsFile coverBmp;
  if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    return false;
  }
  // The rows below are small (a 1-bit 480px row is 60 bytes) and the header goes out in 2- and
  // 4-byte fields, so unbuffered this was well over a thousand file calls. See BufferedPrint.
  BufferedPrint out(coverBmp);

  const uint8_t padding[4] = {0, 0, 0, 0};

  if (bitDepth == 2) {
    // XTH 2-bit: emit a 2-bpp grayscale BMP so the four XTH levels survive to the
    // cover, rather than thresholding to pure black/white. (Original 1-bit code
    // collapsed dark gray / light gray / black all to black.) Issue identified by
    // GitHub user itsthisjustin in crosspoint-reader PR #2335.
    //
    // The Bitmap reader maps a 2-bpp pixel as paletteLum[index] >> 6, producing
    // output color 0=black .. 3=white. We therefore pick a 4-entry palette of the
    // native gray levels and map each XTH value to the palette index whose
    // luminance reproduces it: XTH 0=white, 1=dark gray, 2=light gray, 3=black.
    static constexpr uint8_t kXthToBmp[4] = {3, 1, 2, 0};

    // 2-bpp BMP: 40-byte info header + 4-entry palette = 70 bytes before pixels.
    const uint32_t paletteSize = 4u * 4u;  // 4 RGBQUAD entries
    const uint32_t offBits = 14u + 40u + paletteSize;
    const uint32_t rowSize = ((static_cast<uint32_t>(pageInfo.width) * 2 + 31) / 32) * 4;
    const size_t dstRowSize = (pageInfo.width + 3) / 4;  // 2-bit packed source bytes
    const size_t paddingSize = rowSize - dstRowSize;
    const uint32_t imageSize = rowSize * pageInfo.height;

    auto writeLE16 = [&](uint16_t v) {
      const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
      out.write(b, 2);
    };
    auto writeLE32 = [&](uint32_t v) {
      const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                            static_cast<uint8_t>(v >> 24)};
      out.write(b, 4);
    };

    // BITMAPFILEHEADER
    out.write(reinterpret_cast<const uint8_t*>("BM"), 2);
    writeLE32(offBits + imageSize);  // bfSize
    writeLE16(0);                    // bfReserved1
    writeLE16(0);                    // bfReserved2
    writeLE32(offBits);              // bfOffBits
    // BITMAPINFOHEADER
    writeLE32(40);                                       // biSize
    writeLE32(static_cast<uint32_t>(pageInfo.width));    // biWidth
    writeLE32(static_cast<uint32_t>(-pageInfo.height));  // biHeight (negative = top-down)
    writeLE16(1);                                        // biPlanes
    writeLE16(2);                                        // biBitCount
    writeLE32(0);                                        // biCompression (BI_RGB)
    writeLE32(imageSize);                                // biSizeImage
    writeLE32(2835);                                     // biXPelsPerMeter (~72 DPI)
    writeLE32(2835);                                     // biYPelsPerMeter
    writeLE32(4);                                        // biClrUsed
    writeLE32(4);                                        // biClrImportant
    // Palette: black, dark gray, light gray, white (BGRA)
    const uint8_t palette[16] = {0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x00,
                                 0xAA, 0xAA, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
    out.write(palette, sizeof(palette));

    // Stream the column-major page through a row-major transpose (small RAM band +
    // temp file) instead of holding the whole page in heap.
    XthRowTranspose xth(mutParser, 0, pageInfo.width, pageInfo.height, cachePath + "/cover.t2bit");
    if (!xth.ok) {
      out.discard();  // the file is about to be deleted; do not flush into it
      coverBmp.close();
      Storage.remove(getCoverBmpPath().c_str());
      return false;
    }

    uint8_t* packedRow = static_cast<uint8_t*>(malloc(xth.packedRowBytes));  // freed below
    uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(dstRowSize));          // 2-bit BMP row, freed below
    if (!packedRow || !rowBuffer) {
      LOG_ERR("XTC", "OOM cover row buffers (%d + %u bytes)", xth.packedRowBytes, (unsigned)dstRowSize);
      free(packedRow);
      free(rowBuffer);
      out.discard();  // the file is about to be deleted; do not flush into it
      coverBmp.close();
      Storage.remove(getCoverBmpPath().c_str());
      return false;
    }

    bool failed = false;
    for (uint16_t y = 0; y < pageInfo.height; y++) {
      if (!xth.readRow(y, packedRow)) {
        failed = true;
        break;
      }
      memset(rowBuffer, 0, dstRowSize);
      for (uint16_t x = 0; x < pageInfo.width; x++) {
        const uint8_t bmpVal = kXthToBmp[packedPixel(packedRow, x)];
        rowBuffer[(x * 2) / 8] |= static_cast<uint8_t>(bmpVal << (6 - ((x * 2) % 8)));
      }
      out.write(rowBuffer, dstRowSize);
      if (paddingSize > 0) out.write(padding, paddingSize);
    }
    free(packedRow);
    free(rowBuffer);
    if (failed) {
      out.discard();  // the file is about to be deleted; do not flush into it
      coverBmp.close();
      Storage.remove(getCoverBmpPath().c_str());
      return false;
    }
  } else {
    // 1-bit source: write a 1-bit BMP header and stream source rows straight to it.
    BmpHeader bmpHeader;
    createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
    out.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

    const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;
    const size_t srcRowSize = (pageInfo.width + 7) / 8;  // packed 1-bit source/dest row
    const size_t paddingSize = rowSize - srcRowSize;

    uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(srcRowSize));  // freed before every return below
    if (!rowBuffer) {
      LOG_ERR("XTC", "OOM cover row buffer (%u bytes)", (unsigned)srcRowSize);
      out.discard();  // the file is about to be deleted; do not flush into it
      coverBmp.close();
      Storage.remove(getCoverBmpPath().c_str());
      return false;
    }

    bool failed = false;
    for (uint16_t y = 0; y < pageInfo.height; y++) {
      if (mutParser->loadPageRange(0, static_cast<size_t>(y) * srcRowSize, rowBuffer, srcRowSize) != srcRowSize) {
        failed = true;
        break;
      }
      out.write(rowBuffer, srcRowSize);
      if (paddingSize > 0) out.write(padding, paddingSize);
    }
    mutParser->endPageRange();
    free(rowBuffer);
    if (failed) {
      out.discard();  // the file is about to be deleted; do not flush into it
      coverBmp.close();
      Storage.remove(getCoverBmpPath().c_str());
      return false;
    }
  }

  // Flush before the close, so the destructor never writes into a closed handle -- and so a
  // write that fails at the very end is not reported as a complete cover.
  if (!out.flushBuffer()) {
    LOG_ERR("XTC", "Failed to flush buffered cover BMP");
    coverBmp.close();
    Storage.remove(getCoverBmpPath().c_str());
    return false;
  }
  coverBmp.close();
  LOG_DBG("XTC", "Generated cover BMP: %s", getCoverBmpPath().c_str());
  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }
std::string Xtc::getThumbBmpPath(int width, int height) const {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

bool Xtc::generateThumbBmp(int height) const {
  const std::string destPath = getThumbBmpPath(height);
  if (Storage.exists(destPath.c_str())) return true;
  const int width = static_cast<int>(height * 0.6f);
  if (!generateThumbBmp(width, height)) return false;
  const std::string srcPath = getThumbBmpPath(width, height);
  Storage.rename(srcPath.c_str(), destPath.c_str());
  return Storage.exists(destPath.c_str());
}

bool Xtc::generateThumbBmp(int width, int height) const {
  if (Storage.exists(getThumbBmpPath(width, height).c_str())) return true;

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }
  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  setupCacheDir();

  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  const uint8_t bitDepth = parser->getBitDepth();
  const int THUMB_TARGET_WIDTH = width;
  const int THUMB_TARGET_HEIGHT = height;

  float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;

  if (scale >= 1.0f) {
    if (generateCoverBmp()) {
      return Storage.copyFile("XTC", getCoverBmpPath(), getThumbBmpPath(width, height).c_str());
    }
    return false;
  }

  uint16_t thumbWidth = static_cast<uint16_t>(pageInfo.width * scale);
  uint16_t thumbHeight = static_cast<uint16_t>(pageInfo.height * scale);

  xtc::XtcParser* mutParser = const_cast<xtc::XtcParser*>(parser.get());
  const std::string thumbPath = getThumbBmpPath(width, height);

  // Source pixel access is streamed instead of holding the whole page in heap.
  // The box-downscale sweeps a small band of source rows ([srcYStart, srcYEnd))
  // repeatedly across every output column, so we cache that whole band of source
  // rows in RAM and only refill it as the band slides down (dstY is monotonic).
  // A per-pixel file read here would re-fetch the same rows thousands of times.
  //
  // XTH (column-major): transpose once into a row-major temp, then read rows.
  // XTG (row-major): read source rows directly from the page.
  //
  // The band stores one grayscale byte per source pixel (0=black .. 255=white),
  // preserving the 4 gray levels of XTH for the box average. Band size is
  // bandCap * width bytes (~a couple KB), versus the full ~94KB page it replaces.
  const size_t srcRowBytes = (pageInfo.width + 7) / 8;     // packed 1-bit page row (XTG read unit)
  const int bandCap = static_cast<int>(1.0f / scale) + 2;  // rows spanned by one dstY + slack
  uint8_t* srcBand = static_cast<uint8_t*>(malloc(static_cast<size_t>(bandCap) * pageInfo.width));  // freed below
  uint8_t* packRow = static_cast<uint8_t*>(malloc(srcRowBytes));  // scratch for one packed XTG row
  if (!srcBand || !packRow) {
    LOG_ERR("XTC", "OOM thumb src band (%u bytes)", (unsigned)(bandCap * pageInfo.width + srcRowBytes));
    free(srcBand);
    free(packRow);
    return false;
  }
  int bandStartY = -1;  // first source row currently held in srcBand; -1 = empty
  int bandCount = 0;    // rows currently held

  std::unique_ptr<XthRowTranspose> xth;
  if (bitDepth == 2) {
    xth.reset(new (std::nothrow) XthRowTranspose(mutParser, 0, pageInfo.width, pageInfo.height, thumbPath + ".t2bit"));
    if (!xth || !xth->ok) {
      free(srcBand);
      free(packRow);
      return false;
    }
  }

  // Ensure source rows [need0, need1) are resident in srcBand as grayscale bytes.
  // Each needed row is read/decoded exactly once. Returns false on read error.
  auto ensureBand = [&](int need0, int need1) -> bool {
    if (bandStartY == need0 && bandCount >= (need1 - need0)) return true;
    bandStartY = need0;
    bandCount = need1 - need0;
    for (int i = 0; i < bandCount; i++) {
      const int srcY = need0 + i;
      uint8_t* dst = srcBand + static_cast<size_t>(i) * pageInfo.width;
      if (bitDepth == 1) {
        if (mutParser->loadPageRange(0, static_cast<size_t>(srcY) * srcRowBytes, packRow, srcRowBytes) != srcRowBytes) {
          return false;
        }
        for (uint16_t x = 0; x < pageInfo.width; x++) {
          // XTG: bit 1 = white (255), bit 0 = black (0)
          dst[x] = ((packRow[x / 8] >> (7 - (x % 8))) & 1) ? 255 : 0;
        }
      } else {
        // XTH levels: 0=white, 1=dark gray, 2=light gray, 3=black. A plain
        // (3 - v) * 85 would swap dark and light gray, so map through a table that
        // matches the cover encoding. (Mapping fix from crosspoint-reader PR #2335,
        // itsthisjustin.)
        static constexpr uint8_t kXthToGray[4] = {255, 85, 170, 0};
        for (uint16_t x = 0; x < pageInfo.width; x++) {
          dst[x] = kXthToGray[xth->pixelValue(x, srcY)];
        }
      }
    }
    return true;
  };

  // Returns source grayscale (0=black .. 255=white) at (srcX, srcY) from the band.
  auto srcGray = [&](uint16_t srcX, uint16_t srcY) -> uint8_t {
    return srcBand[static_cast<size_t>(srcY - bandStartY) * pageInfo.width + srcX];
  };

  auto cleanupSrc = [&]() {
    if (bitDepth == 1) mutParser->endPageRange();
    free(srcBand);
    free(packRow);
  };

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", thumbPath, thumbBmp)) {
    cleanupSrc();
    return false;
  }

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(BmpHeader));

  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));  // freed below
  if (!rowBuffer) {
    cleanupSrc();
    thumbBmp.close();
    Storage.remove(thumbPath.c_str());
    return false;
  }

  uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);

  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer, 0xFF, rowSize);
    uint32_t srcYStart = (static_cast<uint32_t>(dstY) * scaleInv_fp) >> 16;
    uint32_t srcYEnd = (static_cast<uint32_t>(dstY + 1) * scaleInv_fp) >> 16;
    if (srcYStart >= pageInfo.height) srcYStart = pageInfo.height - 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
    if (srcYEnd <= srcYStart) srcYEnd = srcYStart + 1;
    if (srcYEnd > pageInfo.height) srcYEnd = pageInfo.height;
    // Clamp the averaged band to bandCap rows (matches the scale estimate; only
    // pathological rounding could exceed it, and dropping extra rows is harmless).
    if (static_cast<int>(srcYEnd - srcYStart) > bandCap) srcYEnd = srcYStart + bandCap;

    if (!ensureBand(static_cast<int>(srcYStart), static_cast<int>(srcYEnd))) {
      LOG_ERR("XTC", "Thumb band read error at dstY %u", dstY);
      free(rowBuffer);
      thumbBmp.close();
      Storage.remove(thumbPath.c_str());
      cleanupSrc();
      return false;
    }

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      uint32_t srcXStart = (static_cast<uint32_t>(dstX) * scaleInv_fp) >> 16;
      uint32_t srcXEnd = (static_cast<uint32_t>(dstX + 1) * scaleInv_fp) >> 16;
      if (srcXStart >= pageInfo.width) srcXStart = pageInfo.width - 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;
      if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
      if (srcXEnd > pageInfo.width) srcXEnd = pageInfo.width;

      uint32_t graySum = 0, totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          graySum += srcGray(static_cast<uint16_t>(srcX), static_cast<uint16_t>(srcY));
          totalCount++;
        }
      }

      uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;
      uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);
      uint8_t oneBit = (avgGray >= adjustedThreshold) ? 1 : 0;
      const size_t byteIndex = dstX / 8;
      const size_t bitOffset = 7 - (dstX % 8);
      if (byteIndex < rowSize) {
        if (oneBit)
          rowBuffer[byteIndex] |= (1 << bitOffset);
        else
          rowBuffer[byteIndex] &= ~(1 << bitOffset);
      }
    }
    thumbBmp.write(rowBuffer, rowSize);
  }

  free(rowBuffer);
  thumbBmp.close();
  cleanupSrc();
  LOG_DBG("XTC", "Generated thumb BMP (%dx%d): %s", thumbWidth, thumbHeight, getThumbBmpPath(width, height).c_str());
  return true;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

std::string Xtc::pageBwCachePath(uint32_t pageIndex) const {
  return cachePath + "/page_" + std::to_string(pageIndex) + ".bw1";
}

std::string Xtc::pageGrayCachePath(uint32_t pageIndex) const {
  return cachePath + "/page_" + std::to_string(pageIndex) + ".gr2";
}

void Xtc::clearPageCache() const {
  // Remove transposed page planes left over from a prior session so the on-disk
  // cache stays bounded (the in-memory LRU only tracks the current session).
  if (!Storage.exists(cachePath.c_str())) return;
  for (const auto& name : Storage.listFiles(cachePath.c_str(), 200)) {
    if (name.startsWith("page_") && (name.endsWith(".bw1") || name.endsWith(".gr2"))) {
      Storage.remove((cachePath + "/" + name.c_str()).c_str());
    }
  }
  pageCacheLru.clear();
}

void Xtc::touchPageCache(uint32_t pageIndex) const {
  // Move to most-recent; drop any stale duplicate entry first.
  for (auto it = pageCacheLru.begin(); it != pageCacheLru.end(); ++it) {
    if (*it == pageIndex) {
      pageCacheLru.erase(it);
      break;
    }
  }
  pageCacheLru.push_back(pageIndex);
  while (pageCacheLru.size() > kPageCacheMax) {
    const uint32_t evict = pageCacheLru.front();
    pageCacheLru.pop_front();
    Storage.remove(pageBwCachePath(evict).c_str());
    Storage.remove(pageGrayCachePath(evict).c_str());
  }
}

bool Xtc::openPageRowStream(xtc::XtcPageRowStream& stream, uint32_t pageIndex, bool persist) const {
  if (!loaded || !parser) {
    return false;
  }
  setupCacheDir();
  // Persist (reader) keeps the transposed planes so revisiting a page is a pure
  // read; the LRU caps how many pages we keep on disk. Non-persist (one-shot
  // preview) uses a scratch name and drops the files on close.
  const std::string bwPath = persist ? pageBwCachePath(pageIndex) : (cachePath + "/page_preview.bw1");
  const std::string grayPath = persist ? pageGrayCachePath(pageIndex) : (cachePath + "/page_preview.gr2");
  const bool ok = stream.open(const_cast<xtc::XtcParser*>(parser.get()), pageIndex, parser->getWidth(),
                              parser->getHeight(), parser->getBitDepth(), bwPath, grayPath, /*keep=*/persist);
  if (ok && persist && parser->getBitDepth() == 2) {
    touchPageCache(pageIndex);  // only XTH writes cache files worth tracking
  }
  return ok;
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}