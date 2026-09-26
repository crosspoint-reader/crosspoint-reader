#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  static constexpr uint8_t TXT_CACHE_VERSION = 1;
  static constexpr uint8_t MD_CACHE_VERSION = 1;

  static bool isTxtOrMd(std::string_view path);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  static bool streamTxtToHtml(const std::string& filepath, Print& out);
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache);
};
