#include "Txt.h"

#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Serialization.h>
#include <Utf8.h>

#include <cstring>

#include "TxtToHtml.h"

bool Txt::isTxtOrMd(std::string_view path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

std::string Txt::findCompanionCoverImage(const std::string& filepath) {
  std::string folder = FsHelpers::extractFolderPath(filepath);

  std::string baseName = FsHelpers::getFileNameWithoutExtension(filepath);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) return coverPath;
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) return coverPath;
    }
  }

  return "";
}

bool Txt::convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight,
                                 bool cropped, bool originalThresholds) {
  if (!Storage.exists(imagePath.c_str())) return false;

  const bool isBmp = FsHelpers::hasBmpExtension(imagePath);
  const bool isJpg = FsHelpers::hasJpgExtension(imagePath);
  const bool isPng = FsHelpers::hasPngExtension(imagePath);
  if (!isBmp && !isJpg && !isPng) return false;

  HalFile src, dst;
  if (!Storage.openFileForRead("TXT", imagePath, src) || !Storage.openFileForWrite("TXT", destBmpPath, dst)) {
    return false;
  }

  bool success = false;
  if (isBmp) {
    uint8_t buf[128];
    int n;
    success = true;
    while ((n = src.read(buf, sizeof(buf))) > 0) {
      if (dst.write(buf, n) != static_cast<size_t>(n)) {
        success = false;
        break;
      }
    }
    if (n < 0) {
      success = false;
    }
  } else if (thumbHeight > 0) {
    const int targetWidth = thumbHeight * 0.6;
    const int targetHeight = thumbHeight;
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    }
  } else {
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileToBmpStream(src, dst, cropped, originalThresholds);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileToBmpStream(src, dst, cropped, originalThresholds);
    }
  }

  src.close();
  dst.close();

  if (!success) {
    Storage.remove(destBmpPath.c_str());
  }

  return success;
}

bool Txt::streamTxtToHtml(const std::string& filepath, Print& out) {
  const uint32_t t0 = millis();
  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    LOG_ERR("TXT", "Failed to open TXT/MD for streaming: %s", filepath.c_str());
    return false;
  }

  const size_t srcSize = src.size();
  LOG_DBG("TXT", "Converting TXT/MD to HTML: %s (%zu bytes)", filepath.c_str(), srcSize);

  const bool ok = TxtToHtml::stream(
      filepath, &src, [](void* ctx, uint8_t* buf, size_t size) { return static_cast<HalFile*>(ctx)->read(buf, size); },
      out);

  LOG_DBG("TXT", "Converted TXT/MD to HTML in %lu ms (%zu bytes in)", millis() - t0, srcSize);
  return ok;
}

void Txt::invalidateCache(const std::string& cachePath) {
  Storage.removeDir((cachePath + "/html").c_str());
  Storage.removeDir((cachePath + "/sections").c_str());
}

bool Txt::validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize) {
  bool valid = true;

  // 1. If html/0.html exists, check its embedded version comment
  const std::string htmlPath = cachePath + "/html/0.html";
  if (Storage.exists(htmlPath.c_str())) {
    HalFile htmlFile;
    if (Storage.openFileForRead("TXT", htmlPath, htmlFile)) {
      char header[80] = {0};
      const int bytesRead = htmlFile.read(header, sizeof(header) - 1);
      if (bytesRead > 0) {
        header[bytesRead] = '\0';
        if (strstr(header, TxtToHtml::cacheVersionTag(filepath)) == nullptr) {
          LOG_DBG("TXT", "HTML cache version mismatch or missing, invalidating: %s", htmlPath.c_str());
          valid = false;
        }
      } else {
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  // 2. Check if source file size changed
  if (valid && cachedSize > 0) {
    HalFile rawFile;
    if (Storage.openFileForRead("TXT", filepath, rawFile)) {
      if (rawFile.size() != cachedSize) {
        LOG_DBG("TXT", "File size changed (%u vs cached %u), invalidating cache", static_cast<uint32_t>(rawFile.size()),
                static_cast<uint32_t>(cachedSize));
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  if (!valid) {
    LOG_DBG("TXT", "Cache invalid for %s, wiping html and sections", filepath.c_str());
    invalidateCache(cachePath);
  }

  return valid;
}

bool Txt::buildTxtCache(const std::string& filepath, const std::string& cachePath,
                        std::unique_ptr<BookMetadataCache>& bookMetadataCache) {
  LOG_DBG("TXT", "Building metadata cache for TXT: %s", filepath.c_str());

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  } else {
    invalidateCache(cachePath);
  }

  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("TXT", "Could not begin writing cache");
    return false;
  }

  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("TXT", "Could not begin writing content.opf pass");
    return false;
  }

  bookMetadataCache->createSpineEntry("content.html");

  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("TXT", "Could not end writing content.opf pass");
    return false;
  }

  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("TXT", "Could not begin writing toc pass");
    return false;
  }

  std::string title = FsHelpers::getFileNameWithoutExtension(filepath);
  bookMetadataCache->createTocEntry(title, "content.html", "", 0);

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("TXT", "Could not end writing toc pass");
    return false;
  }

  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("TXT", "Could not end writing cache");
    return false;
  }

  BookMetadataCache::BookMetadata bookMetadata;
  bookMetadata.title = utf8ComposeNfc(title);
  bookMetadata.language = "en";

  std::string companionCover = findCompanionCoverImage(filepath);
  if (!companionCover.empty()) {
    bookMetadata.coverItemHref = companionCover;
  }

  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("TXT", "Could not build book.bin for TXT");
    return false;
  }

  bookMetadataCache->cleanupTmpFiles();

  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache || !bookMetadataCache->load()) {
    LOG_ERR("TXT", "Failed to reload cache after build");
    return false;
  }

  return true;
}
