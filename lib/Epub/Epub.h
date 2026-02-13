#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void parseCssFiles() const;
  std::string getCssRulesCache() const;
  bool loadCssRulesFromCache() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  // Generate a 1-bit BMP cover image from the EPUB cover image.
  // Returns true on success. On conversion failure, callers may use
  // `generateInvalidFormatCoverBmp` to create a valid marker BMP.
  bool generateCoverBmp(bool cropped = false) const;
  // Create a valid 1-bit BMP that visually indicates an invalid/unsupported
  // cover format (an X pattern). This prevents repeated generation attempts
  // by providing a valid BMP file that `isValidThumbnailBmp` accepts.
  bool generateInvalidFormatCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  // Generate a thumbnail BMP at the requested `height`. Returns true on
  // successful conversion. If conversion fails, `generateInvalidFormatThumbBmp`
  // can be used to write a valid marker image that prevents retries.
  bool generateThumbBmp(int height) const;
  // Create a valid 1-bit thumbnail BMP with an X marker indicating an
  // invalid/unsupported cover image instead of leaving an empty marker file.
  bool generateInvalidFormatThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  const CssParser* getCssParser() const { return cssParser.get(); }

  static bool isValidThumbnailBmp(const std::string& bmpPath);

 private:
  std::vector<std::string> getCoverCandidates() const;
};
