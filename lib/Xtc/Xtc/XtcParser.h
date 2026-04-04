/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossPoint Reader
 */

#pragma once

#include <HalStorage.h>

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "XtcTypes.h"

namespace xtc {

/**
 * XTC File Parser
 *
 * Reads XTC files from SD card and extracts page data.
 * Designed for ESP32-C3's limited RAM (~380KB) using streaming.
 */
class XtcParser {
 public:
  XtcParser();
  ~XtcParser();

  // File open/close
  XtcError open(const char* filepath, const char* cacheDir);
  void close();
  bool isOpen() const { return m_isOpen; }

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH

  // Page information - three-tier cache interface
  bool getPageInfo(uint32_t pageIndex, PageInfo& info);

  // Preload window around specified page (optimize sequential page turns)
  void prefetchWindow(uint32_t pageIndex);

  // Load page bitmap (unchanged)
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  const std::vector<ChapterInfo>& getChapters();

  // Validation
  static bool isValidXtcFile(const char* filepath);

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  FsFile m_file;
  FsFile m_cacheFile;
  bool m_isOpen;
  XtcHeader m_header;
  std::string m_cacheDir;
  std::string m_cacheFilePath;
  std::string m_originalPath;
  std::string m_title;
  std::string m_author;
  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;
  bool m_hasChapters;
  bool m_chaptersLoaded = false;
  XtcError m_lastError;
  uint32_t m_accessCounter = 0;

  // L1: Hot cache (fixed 4 entries)
  std::array<L1CacheEntry, L1_CACHE_SIZE> m_l1Cache;

  // L2: Sliding window (fixed size array)
  std::array<PageInfo, L2_WINDOW_SIZE> m_l2Window;
  uint32_t m_l2WindowStart = 0;
  size_t m_l2WindowCount = 0;
  bool m_l2Valid = false;

  // Chapters (usually few, keep in memory)
  std::vector<ChapterInfo> m_chapters;

  // Original Page Table offset (for rebuilding cache)
  uint64_t m_pageTableOffset = 0;

  // Internal helper functions
  XtcError readHeader();
  XtcError readTitle();
  XtcError readAuthor();
  XtcError readChapters();
  void ensureChaptersLoaded();

  // L3 cache management
  bool isPageTableCacheValid() const;
  XtcError buildPageTableCache();
  bool openCacheFile();
  void closeCacheFile();

  // L1/L2 cache operations
  bool lookupL1(uint32_t pageIndex, PageInfo& info);
  void updateL1(uint32_t pageIndex, const PageInfo& info);
  bool lookupL2(uint32_t pageIndex, PageInfo& info);
  void loadL2Window(uint32_t centerPage);

  // Safe deserialization (alignment-safe for ESP32-C3)
  static void safeDeserializeHeader(const uint8_t* buf, PageTableCacheHeader& header);
  static void safeSerializeHeader(uint8_t* buf, const PageTableCacheHeader& header);

  // Utility functions
  uint32_t calculateFileHash(const char* filepath) const;
};

}  // namespace xtc
