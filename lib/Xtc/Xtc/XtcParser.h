/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossPoint Reader
 */

#pragma once

#include <SdFat.h>

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
  XtcError open(const char* filepath);
  void close();
  bool isOpen() const { return m_isOpen; }

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH

  // Page information
  bool getPageInfo(uint32_t pageIndex, PageInfo& info) const;

  /**
   * Load page bitmap (raw 1-bit data, skipping XTG header)
   *
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer (caller allocated)
   * @param bufferSize Buffer size
   * @return Number of bytes read on success, 0 on failure
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);


/**
 * @brief 动态加载下一批页
 * @return XtcError 加载状态：OK=加载成功，PAGE_OUT_OF_RANGE=无更多页可加载，其他=加载失败
 */
XtcError loadNextPageBatch();

/**
 * @brief 获取当前已经加载的最大页码
 * @return uint16_t 当前加载的最大有效页码
 */
uint16_t getLoadedMaxPage() const;

/**
 * @brief 获取每次动态加载的页数（批次大小）
 * @return uint16_t 批次页数，默认10
 */
uint16_t getPageBatchSize() const;

uint32_t getChapterstartpage(int chapterIndex) {
    for(int i = 0; i < 25; i++) {
        if(ChapterList[i].chapterIndex == chapterIndex) {
            return ChapterList[i].startPage;
        }
    }
    return 0; // 无此章节返回0
}

std::string getChapterTitleByIndex(int chapterIndex) {
    Serial.printf("[%lu] [XTC] 已进入getChapterTitleByIndex，chapterActualCount=%d\n", millis(),chapterActualCount);
    for(int i = 0; i < 25; i++) {
        if(ChapterList[i].chapterIndex == chapterIndex) {
            return std::string(ChapterList[i].shortTitle);
            Serial.printf("[%lu] [XTC] getChapterTitleByIndex里第%d章，名字为:%s %u\n", millis(), i, ChapterList[i].shortTitle);
        }
    }
    return ""; // 无此章节返回空字符串
}



  /**
   * Streaming page load
   * Memory-efficient method that reads page data in chunks.
   *
   * @param pageIndex Page index
   * @param callback Callback function to receive data chunks
   * @param chunkSize Chunk size (default: 1024 bytes)
   * @return Error code
   */
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  const std::vector<ChapterInfo>& getChapters() const { return m_chapters; }

  XtcError readChapters_gd(uint16_t chapterStart);
 ChapterData ChapterList[MAX_SAVE_CHAPTER];
  int chapterActualCount = 0;
  XtcError loadPageBatchByStart(uint16_t startPage);

  // Validation
  static bool isValidXtcFile(const char* filepath);

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  FsFile m_file;
  bool m_isOpen;
  XtcHeader m_header;
  std::vector<PageInfo> m_pageTable;
  std::vector<ChapterInfo> m_chapters;
  std::string m_title;
  std::string m_author;
  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;  // 1 = XTC/XTG (1-bit), 2 = XTCH/XTH (2-bit)
  bool m_hasChapters;
  XtcError m_lastError;

  // Internal helper functions
  XtcError readHeader();
  XtcError readPageTable();
  XtcError readTitle();
  XtcError readAuthor();
  XtcError readChapters();
  uint16_t m_loadBatchSize = 10;    // 每次加载的页数（核心配置，可改）
  uint16_t m_loadedMaxPage = 0;     // 记录当前加载到的最大页码
};

}  // namespace xtc
