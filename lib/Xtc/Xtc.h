/**
 * Xtc.h
 *
 * Main XTC ebook class for CrossPoint Reader
 * Provides EPUB-like interface for XTC file handling
 */

#pragma once

#include <memory>
#include <string>

#include "Xtc/XtcParser.h"
#include "Xtc/XtcTypes.h"

/**
 * XTC Ebook Handler
 *
 * Handles XTC file loading, page access, and cover image generation.
 * Interface is designed to be similar to Epub class for easy integration.
 */
class Xtc {
  std::string filepath;
  std::string cachePath;
  std::unique_ptr<xtc::XtcParser> parser;
  bool loaded;

 public:
  explicit Xtc(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)), loaded(false) {
    // Create cache key based on filepath (same as Epub)
    cachePath = cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Xtc() = default;

  /**
   * Load XTC file
   * @return true on success
   */
  bool load();

  /**
   * Clear cached data
   * @return true on success
   */
  bool clearCache() const;

  /**
   * Setup cache directory
   */
  void setupCacheDir() const;

  // Path accessors
  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  // Metadata
  std::string getTitle() const;

  // Cover image support (for sleep screen)
  std::string getCoverBmpPath() const;
  bool generateCoverBmp() const;

  // Page access
  uint32_t getPageCount() const;
  uint16_t getPageWidth() const;
  uint16_t getPageHeight() const;
  uint8_t getBitDepth() const;  // 1 = XTC (1-bit), 2 = XTCH (2-bit)

  /**
   * Load page bitmap data
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer
   * @param bufferSize Buffer size
   * @return Number of bytes read
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const;

  /**
   * Load page with streaming callback
   * @param pageIndex Page index
   * @param callback Callback for each chunk
   * @param chunkSize Chunk size
   * @return Error code
   */
  xtc::XtcError loadPageStreaming(uint32_t pageIndex,
                                  std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                  size_t chunkSize = 1024) const;

  // Progress calculation
  uint8_t calculateProgress(uint32_t currentPage) const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

  // Error information
  xtc::XtcError getLastError() const;
};
