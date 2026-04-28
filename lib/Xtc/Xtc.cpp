/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

static inline void computeSrcRange(uint32_t dstCoord, uint32_t scaleInv, uint32_t maxSrc, uint32_t& srcStart,
                                   uint32_t& srcEnd) {
  srcStart = (static_cast<uint32_t>(dstCoord) * scaleInv) >> 16;
  srcEnd = (static_cast<uint32_t>(dstCoord + 1) * scaleInv) >> 16;
  if (srcStart >= maxSrc) srcStart = maxSrc - 1;
  if (srcEnd > maxSrc) srcEnd = maxSrc;
  if (srcEnd <= srcStart) srcEnd = srcStart + 1;
  if (srcEnd > maxSrc) srcEnd = maxSrc;
}

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

std::string Xtc::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Xtc::generateCoverBmp() const {
  // Already generated
  if (Storage.exists(getCoverBmpPath().c_str())) {
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

  // --- 2-bit XTCH/XTH: two-pass plane loading to stay within heap limits ---
  // Each plane is ~48KB (fits MaxAlloc); both planes together (~96KB) do not.
  if (bitDepth == 2) {
    const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
    const size_t colBytes = (pageInfo.height + 7) / 8;
    const uint32_t rowSize2 = ((static_cast<uint32_t>(pageInfo.width) * 2 + 31) / 32) * 4;
    const std::string tempPath = cachePath + "/cover_tmp.bmp";

    // Pass 1: load plane1, write raw 2-bit rows to temp file (bit2=0, pixel = bit1<<1)
    {
      uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane1) {
        LOG_ERR("XTC", "Failed to alloc plane1 (%lu bytes)", planeSize);
        return false;
      }
      if (const_cast<xtc::XtcParser*>(parser.get())->loadPageMsb(0, plane1, planeSize) == 0) {
        LOG_ERR("XTC", "Failed to load plane1 for cover");
        free(plane1);
        return false;
      }
      FsFile tempFile;
      if (!Storage.openFileForWrite("XTC", tempPath, tempFile)) {
        LOG_ERR("XTC", "Failed to open temp cover file");
        free(plane1);
        return false;
      }
      uint8_t rowBuf[256];
      for (uint16_t y = 0; y < pageInfo.height; y++) {
        memset(rowBuf, 0, rowSize2);
        for (uint16_t x = 0; x < pageInfo.width; x++) {
          const size_t bo = (pageInfo.width - 1 - x) * colBytes + y / 8;
          const uint8_t bit1 = (plane1[bo] >> (7 - (y % 8))) & 1;
          rowBuf[x / 4] |= static_cast<uint8_t>((bit1 << 1) << (6 - (x % 4) * 2));
        }
        if (tempFile.write(rowBuf, rowSize2) != rowSize2) {
          LOG_ERR("XTC", "Failed to write temp cover row %u", y);
          tempFile.close();
          free(plane1);
          Storage.remove(tempPath.c_str());
          return false;
        }
      }
      tempFile.close();
      free(plane1);
    }

    // Pass 2: load plane2, combine with pass1, write final 2-bit BMP
    {
      uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane2) {
        LOG_ERR("XTC", "Failed to alloc plane2 (%lu bytes)", planeSize);
        Storage.remove(tempPath.c_str());
        return false;
      }
      if (const_cast<xtc::XtcParser*>(parser.get())->loadPageLsb(0, plane2, planeSize) == 0) {
        LOG_ERR("XTC", "Failed to load plane2 for cover");
        free(plane2);
        Storage.remove(tempPath.c_str());
        return false;
      }
      FsFile tempFile, coverFile;
      if (!Storage.openFileForRead("XTC", tempPath, tempFile)) {
        free(plane2);
        Storage.remove(tempPath.c_str());
        return false;
      }
      if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverFile)) {
        tempFile.close();
        free(plane2);
        Storage.remove(tempPath.c_str());
        return false;
      }
      // Write 2-bit BMP header
      const uint32_t imageSize2 = rowSize2 * pageInfo.height;
      const uint32_t fileSize2 = 14 + 40 + 16 + imageSize2;
      static constexpr uint8_t bmpHeader2[74] = {
          'B',  'M',  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    40,   0,   0, 0, 0,
          0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1,    0,    2,    0,    0,   0, 0, 0,
          0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    4,    0,    0,    0,   4, 0, 0,
          0xFF, 0xFF, 0xFF, 0x00, 0xAA, 0xAA, 0xAA, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00};
      uint8_t hdr[74];
      memcpy(hdr, bmpHeader2, 74);
      memcpy(hdr + 2, &fileSize2, 4);
      const uint32_t doff2 = 14 + 40 + 16;
      memcpy(hdr + 10, &doff2, 4);
      int32_t ww2 = pageInfo.width;
      memcpy(hdr + 18, &ww2, 4);
      int32_t hh2 = -static_cast<int32_t>(pageInfo.height);
      memcpy(hdr + 22, &hh2, 4);
      memcpy(hdr + 34, &imageSize2, 4);
      const uint32_t ppm2 = 2835;
      memcpy(hdr + 38, &ppm2, 4);
      memcpy(hdr + 42, &ppm2, 4);
      if (coverFile.write(hdr, 74) != 74) {
        LOG_ERR("XTC", "Failed to write 2-bit BMP header");
        coverFile.close();
        tempFile.close();
        free(plane2);
        Storage.remove(tempPath.c_str());
        return false;
      }
      // For each row: read pass1 row (bit1 only), OR in bit2, write to final
      uint8_t rowBuf[256];
      for (uint16_t y = 0; y < pageInfo.height; y++) {
        memset(rowBuf, 0, rowSize2);
        if (tempFile.read(rowBuf, rowSize2) != rowSize2) {
          LOG_ERR("XTC", "Failed to read temp cover row %u", y);
          coverFile.close();
          tempFile.close();
          free(plane2);
          Storage.remove(tempPath.c_str());
          return false;
        }
        for (uint16_t x = 0; x < pageInfo.width; x++) {
          const size_t bo = (pageInfo.width - 1 - x) * colBytes + y / 8;
          const uint8_t bit2 = (plane2[bo] >> (7 - (y % 8))) & 1;
          rowBuf[x / 4] |= static_cast<uint8_t>(bit2 << (6 - (x % 4) * 2));
        }
        if (coverFile.write(rowBuf, rowSize2) != rowSize2) {
          LOG_ERR("XTC", "Failed to write cover row %u", y);
          coverFile.close();
          tempFile.close();
          free(plane2);
          Storage.remove(tempPath.c_str());
          return false;
        }
      }
      coverFile.close();
      tempFile.close();
      free(plane2);
      Storage.remove(tempPath.c_str());
    }
    LOG_DBG("XTC", "Generated 2-bit cover BMP: %s", getCoverBmpPath().c_str());
    return true;
  }

  // 1-bit (XTC/XTG) path
  const size_t bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }

  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page");
    free(pageBuffer);
    return false;
  }

  FsFile coverBmp;
  if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    free(pageBuffer);
    return false;
  }

  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
  coverBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;
  const size_t srcRowSize = (pageInfo.width + 7) / 8;

  // Write bitmap data
  // BMP requires 4-byte row alignment
  const size_t dstRowSize = (pageInfo.width + 7) / 8;

  if (bitDepth == 2) {
    const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageInfo.height + 7) / 8;

    uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(dstRowSize));
    if (!rowBuffer) {
      free(pageBuffer);
      return false;
    }

    for (uint16_t y = 0; y < pageInfo.height; y++) {
      memset(rowBuffer, 0xFF, dstRowSize);

      for (uint16_t x = 0; x < pageInfo.width; x++) {
        const size_t colIndex = pageInfo.width - 1 - x;
        const size_t byteInCol = y / 8;
        const size_t bitInByte = 7 - (y % 8);

        const size_t byteOffset = colIndex * colBytes + byteInCol;
        const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
        const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
        const uint8_t pixelValue = (bit1 << 1) | bit2;

        if (pixelValue >= 1) {
          const size_t dstByte = x / 8;
          const size_t dstBit = 7 - (x % 8);
          rowBuffer[dstByte] &= ~(1 << dstBit);
        }
      }

      coverBmp.write(rowBuffer, dstRowSize);

      uint8_t padding[4] = {0, 0, 0, 0};
      size_t paddingSize = rowSize - dstRowSize;
      if (paddingSize > 0) {
        coverBmp.write(padding, paddingSize);
      }
    }

    free(rowBuffer);
  } else {
    for (uint16_t y = 0; y < pageInfo.height; y++) {
      coverBmp.write(pageBuffer + y * srcRowSize, srcRowSize);

      uint8_t padding[4] = {0, 0, 0, 0};
      size_t paddingSize = rowSize - srcRowSize;
      if (paddingSize > 0) {
        coverBmp.write(padding, paddingSize);
      }
    }
  }

  free(pageBuffer);

  LOG_DBG("XTC", "Generated cover BMP: %s", getCoverBmpPath().c_str());
  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Xtc::generateThumbBmp(int height) const {
  // Already generated
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
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

  // Calculate target dimensions for thumbnail (fit within 240x400 Continue Reading card)
  int THUMB_TARGET_WIDTH = height * 0.6;
  int THUMB_TARGET_HEIGHT = height;

  // Calculate scale factor
  float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;  // for cropping

  // Only scale down, never up
  if (scale >= 1.0f) {
    // Page is already small enough, just use cover.bmp
    // Copy cover.bmp to thumb.bmp
    if (generateCoverBmp()) {
      FsFile src, dst;
      if (Storage.openFileForRead("XTC", getCoverBmpPath(), src)) {
        if (Storage.openFileForWrite("XTC", getThumbBmpPath(height), dst)) {
          uint8_t buffer[512];
          while (src.available()) {
            size_t bytesRead = src.read(buffer, sizeof(buffer));
            dst.write(buffer, bytesRead);
          }
        }
      }
      LOG_DBG("XTC", "Copied cover to thumb (no scaling needed)");
      return Storage.exists(getThumbBmpPath(height).c_str());
    }
    return false;
  }

  uint16_t thumbWidth = static_cast<uint16_t>(pageInfo.width * scale);
  uint16_t thumbHeight = static_cast<uint16_t>(pageInfo.height * scale);

  LOG_DBG("XTC", "Generating thumb BMP: %dx%d -> %dx%d (scale: %.3f)", pageInfo.width, pageInfo.height, thumbWidth,
          thumbHeight, scale);

  // For 2-bit (XTCH): strip-based sequential plane loading → area-averaged Atkinson dithered 1-bit BMP.
  // Loads each plane separately (~48KB) in strips to stay within memory constraints after reader sessions.
  // Uses area-averaging (same formula as the 1-bit path) for accurate luminance computation.
  if (bitDepth == 2) {
    const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;
    const size_t colBytes = (pageInfo.height + 7) / 8;
    const uint32_t scaleInv_fp2 = static_cast<uint32_t>(65536.0f / scale);

    uint8_t* planeBuffer = static_cast<uint8_t*>(malloc(planeSize));
    if (!planeBuffer) {
      LOG_ERR("XTC", "Failed to alloc plane buffer (%lu bytes)", static_cast<unsigned long>(planeSize));
      return false;
    }

    const size_t stripBudget = (static_cast<size_t>(thumbWidth) * thumbHeight + 7) / 8;
    const int stripRows = std::min(static_cast<int>(thumbHeight), static_cast<int>(stripBudget / thumbWidth));

    uint8_t* darkCount1Buf = static_cast<uint8_t*>(malloc(static_cast<size_t>(stripRows) * thumbWidth));
    if (!darkCount1Buf) {
      LOG_ERR("XTC", "Failed to alloc darkCount1 buffer (%lu bytes)",
              static_cast<unsigned long>(static_cast<size_t>(stripRows) * thumbWidth));
      free(planeBuffer);
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("XTC", getThumbBmpPath(height), thumbBmp)) {
      free(darkCount1Buf);
      free(planeBuffer);
      return false;
    }

    BmpHeader bmpHeader;
    createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
    thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

    const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
    uint8_t* rowBuf = static_cast<uint8_t*>(malloc(rowSize));
    if (!rowBuf) {
      free(darkCount1Buf);
      free(planeBuffer);
      thumbBmp.close();
      Storage.remove(getThumbBmpPath(height).c_str());
      return false;
    }

    Atkinson1BitDitherer ditherer(thumbWidth);

    for (int stripStart = 0; stripStart < static_cast<int>(thumbHeight); stripStart += stripRows) {
      const int curRows = std::min(stripRows, static_cast<int>(thumbHeight) - stripStart);

      // Pass 1: plane1 (bit1/MSB) — count dark pixels per output pixel
      if (const_cast<xtc::XtcParser*>(parser.get())->loadPageMsb(0, planeBuffer, planeSize) == 0) {
        LOG_ERR("XTC", "Failed to load plane1 for thumb");
        free(rowBuf);
        free(darkCount1Buf);
        free(planeBuffer);
        thumbBmp.close();
        Storage.remove(getThumbBmpPath(height).c_str());
        return false;
      }
      for (int dy = 0; dy < curRows; dy++) {
        const uint16_t dstY = static_cast<uint16_t>(stripStart + dy);
        for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
          uint32_t srcYS, srcYE, srcXS, srcXE;
          computeSrcRange(dstY, scaleInv_fp2, pageInfo.height, srcYS, srcYE);
          computeSrcRange(dstX, scaleInv_fp2, pageInfo.width, srcXS, srcXE);
          uint32_t darkCount = 0;
          for (uint32_t sy = srcYS; sy < srcYE; sy++)
            for (uint32_t sx = srcXS; sx < srcXE; sx++) {
              const size_t bo = (pageInfo.width - 1 - sx) * colBytes + sy / 8;
              if (bo < planeSize) {
                if ((planeBuffer[bo] >> (7 - (sy % 8))) & 1) darkCount++;
              }
            }
          darkCount1Buf[static_cast<size_t>(dy) * thumbWidth + dstX] = static_cast<uint8_t>(darkCount);
        }
      }

      // Pass 2: plane2 (bit2/LSB) — combine with darkCount1 → area-average → dither → write BMP rows
      if (const_cast<xtc::XtcParser*>(parser.get())->loadPageLsb(0, planeBuffer, planeSize) == 0) {
        LOG_ERR("XTC", "Failed to load plane2 for thumb");
        free(rowBuf);
        free(darkCount1Buf);
        free(planeBuffer);
        thumbBmp.close();
        Storage.remove(getThumbBmpPath(height).c_str());
        return false;
      }
      for (int dy = 0; dy < curRows; dy++) {
        memset(rowBuf, 0xFF, rowSize);
        const uint16_t dstY = static_cast<uint16_t>(stripStart + dy);
        for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
          uint32_t srcYS, srcYE, srcXS, srcXE;
          computeSrcRange(dstY, scaleInv_fp2, pageInfo.height, srcYS, srcYE);
          computeSrcRange(dstX, scaleInv_fp2, pageInfo.width, srcXS, srcXE);
          uint32_t darkCount = 0, totalCount = 0;
          for (uint32_t sy = srcYS; sy < srcYE; sy++)
            for (uint32_t sx = srcXS; sx < srcXE; sx++) {
              const size_t bo = (pageInfo.width - 1 - sx) * colBytes + sy / 8;
              if (bo < planeSize) {
                if ((planeBuffer[bo] >> (7 - (sy % 8))) & 1) darkCount++;
                totalCount++;
              }
            }
          const uint32_t dark1 = darkCount1Buf[static_cast<size_t>(dy) * thumbWidth + dstX];
          const int lumSum = static_cast<int>((totalCount - dark1) * 85 + (totalCount - darkCount) * 170);
          const int avgLum = (totalCount > 0) ? (lumSum * 255 / totalCount) / 255 : 255;
          const uint8_t bit = ditherer.processPixel(avgLum, dstX);
          if (!bit) {
            const size_t bi = dstX / 8;
            if (bi < rowSize) rowBuf[bi] &= ~(1 << (7 - (dstX % 8)));
          }
        }
        thumbBmp.write(rowBuf, rowSize);
        ditherer.nextRow();
      }
    }

    free(rowBuf);
    free(darkCount1Buf);
    free(planeBuffer);
    thumbBmp.close();
    LOG_DBG("XTC", "Generated 1-bit thumb BMP with dithering (%dx%d): %s", thumbWidth, thumbHeight,
            getThumbBmpPath(height).c_str());
    return true;
  }

  // 1-bit (XTC/XTG) path
  const size_t bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bitmapSize));
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", bitmapSize);
    return false;
  }
  size_t bytesRead = const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer, bitmapSize);
  if (bytesRead == 0) {
    LOG_ERR("XTC", "Failed to load cover page for thumb");
    free(pageBuffer);
    return false;
  }

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", getThumbBmpPath(height), thumbBmp)) {
    LOG_DBG("XTC", "Failed to create thumb BMP file");
    free(pageBuffer);
    return false;
  }

  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));
  if (!rowBuffer) {
    free(pageBuffer);
    return false;
  }

  Atkinson1BitDitherer ditherer(thumbWidth);
  const uint32_t scaleInv_fp = static_cast<uint32_t>(65536.0f / scale);
  const size_t srcRowBytes = (pageInfo.width + 7) / 8;

  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer, 0xFF, rowSize);
    uint32_t srcYStart, srcYEnd;
    computeSrcRange(dstY, scaleInv_fp, pageInfo.height, srcYStart, srcYEnd);

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      uint32_t srcXStart, srcXEnd;
      computeSrcRange(dstX, scaleInv_fp, pageInfo.width, srcXStart, srcXEnd);

      uint32_t graySum = 0, totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          const size_t byteIdx = srcY * srcRowBytes + srcX / 8;
          if (byteIdx < bitmapSize) {
            graySum += ((pageBuffer[byteIdx] >> (7 - (srcX % 8))) & 1) ? 255 : 0;
            totalCount++;
          }
        }
      }

      const int avgGray = (totalCount > 0) ? static_cast<int>(graySum * 255 / totalCount) / 255 : 255;
      const uint8_t bit = ditherer.processPixel(avgGray, dstX);
      if (!bit) {
        const size_t bi = dstX / 8;
        if (bi < rowSize) rowBuffer[bi] &= ~(1 << (7 - (dstX % 8)));
      }
    }
    thumbBmp.write(rowBuffer, rowSize);
    ditherer.nextRow();
  }

  free(rowBuffer);
  free(pageBuffer);
  LOG_DBG("XTC", "Generated 1-bit thumb BMP with Atkinson dithering (%dx%d): %s", thumbWidth, thumbHeight,
          getThumbBmpPath(height).c_str());
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

size_t Xtc::loadPageMsb(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageMsb(pageIndex, buffer, bufferSize);
}

size_t Xtc::loadPageLsb(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageLsb(pageIndex, buffer, bufferSize);
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
