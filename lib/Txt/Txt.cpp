#include "Txt.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("TXT", "File does not exist: %s", filepath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    LOG_ERR("TXT", "Failed to open file: %s", filepath.c_str());
    return false;
  }

  fileSize = file.size();
  file.close();

  loaded = true;
  LOG_DBG("TXT", "Loaded TXT file: %s (%zu bytes)", filepath.c_str(), fileSize);
  return true;
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt or .md extension
  if (FsHelpers::hasTxtExtension(filename)) {
    filename = filename.substr(0, filename.length() - 4);
  } else if (FsHelpers::hasMarkdownExtension(filename)) {
    filename = filename.substr(0, filename.length() - 3);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      LOG_DBG("TXT", "Found matching cover image: %s", coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        LOG_DBG("TXT", "Found fallback cover image: %s", coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG("TXT", "No cover image found for TXT file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    // Copy BMP file to cache
    LOG_DBG("TXT", "Copying BMP cover image to cache");
    FsFile src, dst;
    if (!Storage.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      src.close();
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    src.close();
    dst.close();
    LOG_DBG("TXT", "Copied BMP cover to cache");
    return true;
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    LOG_DBG("TXT", "Generating BMP from JPG cover image");
    FsFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();

    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from JPG cover image");
      Storage.remove(getCoverBmpPath().c_str());
    } else {
      LOG_DBG("TXT", "Generated BMP from JPG cover image");
    }
    return success;
  } else if (FsHelpers::hasPngExtension(coverImagePath)) {
    LOG_DBG("TXT", "Generating BMP from PNG cover image");
    FsFile coverPng, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverPng)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      coverPng.close();
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp);
    coverPng.close();
    coverBmp.close();

    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath().c_str());
    } else {
      LOG_DBG("TXT", "Generated BMP from PNG cover image");
    }
    return success;
  }

  LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG/PNG)");
  return false;
}

std::string Txt::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Txt::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }
std::string Txt::getThumbBmpPath(int width, int height) const {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

bool Txt::generateThumbBmp(int height) const {
  const std::string destPath = getThumbBmpPath(height);
  if (Storage.exists(destPath.c_str())) return true;
  const int width = static_cast<int>(height * 0.6f);
  if (!generateThumbBmp(width, height)) return false;
  const std::string srcPath = getThumbBmpPath(width, height);
  Storage.rename(srcPath.c_str(), destPath.c_str());
  return Storage.exists(destPath.c_str());
}

bool Txt::generateThumbBmp(int width, int height) const {
  const std::string thumbPath = getThumbBmpPath(width, height);
  if (Storage.exists(thumbPath.c_str())) return true;

  setupCacheDir();

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("TXT", thumbPath, thumbBmp)) return false;

  const uint32_t rowSize = (static_cast<uint32_t>(width) + 31) / 32 * 4;
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, width, height, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(BmpHeader));

  uint8_t* rowBuffer = static_cast<uint8_t*>(malloc(rowSize));
  if (!rowBuffer) {
    thumbBmp.close();
    Storage.remove(thumbPath.c_str());
    return false;
  }

  // Matches the Lyra "no cover" placeholder: 1px border, white top third, black bottom two thirds.
  // Book icon (32x32, 1=white/transparent, 0=black) centered in the white area.
  // In 1-bit BMP: 1=white, 0=black.
  static const uint8_t kIcon[] = {
      0xFF, 0x00, 0x00, 0x1F, 0xFF, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFE, 0x1F, 0xE0, 0x00, 0x02, 0x1F, 0xE0, 0x00, 0x03,
      0x1F, 0xE0, 0x00, 0x03, 0x1F, 0xF0, 0x00, 0x01, 0x1F, 0xF0, 0x00, 0x01, 0x1F, 0xF0, 0x00, 0x01, 0x9F, 0xF8, 0x00,
      0x00, 0x9F, 0xF8, 0x00, 0x00, 0xDF, 0xFC, 0x00, 0x00, 0x6F, 0xFE, 0x00, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x1F, 0xFF,
      0x80, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x0F, 0xFF, 0x00, 0x00,
      0x1F, 0xFE, 0x00, 0x00, 0x3F, 0xFC, 0x00, 0x00, 0x6F, 0xF8, 0x00, 0x00, 0xDF, 0xF8, 0x00, 0x00, 0x9F, 0xF0, 0x00,
      0x01, 0x9F, 0xF0, 0x00, 0x01, 0x1F, 0xE0, 0x00, 0x01, 0x1F, 0xE0, 0x00, 0x03, 0x1F, 0xE0, 0x00, 0x02, 0x1F, 0xE0,
      0x00, 0x02, 0x1F, 0xFF, 0xFF, 0xFE, 0x1F, 0xFF, 0x00, 0x00, 0x1F, 0xFF, 0x00, 0x00, 0x1F};
  static constexpr int kIconSize = 32;
  static constexpr int kIconStride = 4;  // bytes per icon row (32 bits)

  const int splitY = height / 3;  // white above, black below
  const int iconX = (width - kIconSize) / 2;
  const int iconY = (splitY - kIconSize) / 2;

  for (int y = 0; y < height; y++) {
    const bool blackRegion = (y >= splitY);
    memset(rowBuffer, blackRegion ? 0x00 : 0xFF, rowSize);

    // 1px border
    if (y == 0 || y == height - 1) {
      memset(rowBuffer, 0x00, rowSize);
    } else {
      // Left and right border pixels
      rowBuffer[0] &= 0x7F;                                         // clear MSB (x=0)
      rowBuffer[(width - 1) / 8] &= ~(0x80u >> ((width - 1) % 8));  // clear x=width-1

      // Overlay icon row if within icon bounds (icon is on white area)
      const int iconRow = y - iconY;
      if (!blackRegion && iconRow >= 0 && iconRow < kIconSize && iconX >= 0 && iconX + kIconSize <= width) {
        // Icon format: 0=dark pixel, 1=white/transparent. In BMP: 0=black, 1=white.
        // The icon pixels (0=dark) should clear bits in the white BMP region.
        for (int ix = 0; ix < kIconSize; ix++) {
          const int iconByte = iconRow * kIconStride + ix / 8;
          const int iconBit = 7 - (ix % 8);
          const bool iconDark = !((kIcon[iconByte] >> iconBit) & 1);
          if (iconDark) {
            const int bx = iconX + ix;
            rowBuffer[bx / 8] &= ~(0x80u >> (bx % 8));
          }
        }
      }
    }
    thumbBmp.write(rowBuffer, rowSize);
  }

  free(rowBuffer);
  thumbBmp.close();
  LOG_DBG("TXT", "Generated surrogate thumb BMP (%dx%d): %s", width, height, thumbPath.c_str());
  return true;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  file.close();

  return bytesRead > 0;
}
