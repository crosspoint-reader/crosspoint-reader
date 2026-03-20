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

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }

  /// Load EPUB metadata and cache, with optional deferred processing
  /// @param buildIfMissing Build cache from scratch if not found (default: true)
  /// @param skipLoadingCss Skip CSS file parsing (default: false)
  /// @param skipCoverGen Skip cover BMP generation on load; deferred until explicitly needed
  ///                     (default: false). Set to true for fast reading mode on large EPUBs.
  /// @return true if load successful, false otherwise
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false, bool skipCoverGen = false);

  /// Clear all cached data for this EPUB
  bool clearCache() const;

  /// Setup cache directory if it doesn't exist
  void setupCacheDir() const;

  /// Get the cache directory path for this EPUB
  const std::string& getCachePath() const;

  /// Get the EPUB file path
  const std::string& getPath() const;

  /// Get book title from metadata cache
  const std::string& getTitle() const;

  /// Get book author from metadata cache
  const std::string& getAuthor() const;

  /// Get book language from metadata cache
  const std::string& getLanguage() const;

  /// Get path to cover BMP file
  std::string getCoverBmpPath(bool cropped = false) const;

  /// Generate cover BMP from cover image (PNG/JPG)
  /// Returns immediately if already generated
  bool generateCoverBmp(bool cropped = false) const;

  /// Get path to thumbnail BMP (for home screen continue-reading card)
  std::string getThumbBmpPath() const;

  /// Get path to thumbnail BMP of specific height
  std::string getThumbBmpPath(int height) const;

  /// Generate thumbnail BMP from cover image
  bool generateThumbBmp(int height) const;

  /// Read EPUB item (chapter, image, etc.) into memory
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;

  /// Stream EPUB item to output (for large files)
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;

  /// Get uncompressed size of EPUB item
  bool getItemSize(const std::string& itemHref, size_t* size) const;

  /// Get spine entry (chapter) at index
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;

  /// Get table of contents entry at index
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;

  /// Get total number of spine items (chapters)
  int getSpineItemsCount() const;

  /// Get total number of TOC items (bookmarks)
  int getTocItemsCount() const;

  /// Convert TOC index to spine index
  int getSpineIndexForTocIndex(int tocIndex) const;

  /// Convert spine index to TOC index
  int getTocIndexForSpineIndex(int spineIndex) const;

  /// Get cumulative size up to spine index
  size_t getCumulativeSpineItemSize(int spineIndex) const;

  /// Get spine index for text reference (start reading position)
  int getSpineIndexForTextReference() const;

  /// Get total book size in bytes
  size_t getBookSize() const;

  /// Calculate reading progress (0.0 to 1.0)
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;

  /// Get CSS parser for styling
  CssParser* getCssParser() const { return cssParser.get(); }

  /// Resolve link HREF to spine index (for navigation)
  int resolveHrefToSpineIndex(const std::string& href) const;
};
