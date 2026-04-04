/**
 * XtcParser.cpp
 *
 * XTC file parsing implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "XtcParser.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <limits>

namespace xtc {

namespace {

constexpr size_t MAX_CHAPTERS = 4096;

bool canSeekToOffset(const uint64_t offset) {
  return offset <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool seekToOffset(FsFile& file, const uint64_t offset) {
  if (!canSeekToOffset(offset)) {
    return false;
  }
  return file.seek(static_cast<size_t>(offset));
}

}  // namespace

void XtcParser::safeDeserializeHeader(const uint8_t* buf, PageTableCacheHeader& header) {
  memcpy(&header.magic, buf + 0, 4);
  memcpy(&header.version, buf + 4, 4);
  memcpy(&header.pageCount, buf + 8, 4);
  memcpy(&header.originalHash, buf + 12, 4);
  memcpy(&header.originalSize, buf + 16, 8);
  memcpy(&header.entrySize, buf + 24, 4);
  memcpy(&header.reserved, buf + 28, 4);
}

void XtcParser::safeSerializeHeader(uint8_t* buf, const PageTableCacheHeader& header) {
  memcpy(buf + 0, &header.magic, 4);
  memcpy(buf + 4, &header.version, 4);
  memcpy(buf + 8, &header.pageCount, 4);
  memcpy(buf + 12, &header.originalHash, 4);
  memcpy(buf + 16, &header.originalSize, 8);
  memcpy(buf + 24, &header.entrySize, 4);
  memcpy(buf + 28, &header.reserved, 4);
}

XtcParser::XtcParser()
    : m_isOpen(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_lastError(XtcError::OK) {
  memset(&m_header, 0, sizeof(m_header));

  for (auto& entry : m_l1Cache) {
    entry.pageIndex = 0xFFFFFFFF;
    entry.lastAccess = 0;
  }
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::open(const char* filepath, const char* cacheDir) {
  // Close any previous file state before reopening
  if (m_isOpen) {
    close();
  }

  m_originalPath = filepath;
  m_cacheDir = cacheDir;

  uint32_t fileHash = calculateFileHash(filepath);
  m_cacheFilePath = std::string(cacheDir) + "/xtc_" + std::to_string(fileHash) + "/page_table.bin";

  // Open the original XTC file just long enough to read metadata and validate the header.
  if (!Storage.openFileForRead("XTC", filepath, m_file)) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Read header
  m_lastError = readHeader();
  if (m_lastError != XtcError::OK) {
    LOG_DBG("XTC", "Failed to read header: %s", errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  if (m_header.pageCount == 0) {
    LOG_ERR("XTC", "File has no pages");
    m_file.close();
    m_lastError = XtcError::CORRUPTED_HEADER;
    return m_lastError;
  }

  // Metadata strings are small, so keep them in memory even when the page table is moved to cache.
  if (m_header.hasMetadata) {
    readTitle();
    readAuthor();
    m_title.shrink_to_fit();
    m_author.shrink_to_fit();
    LOG_INF("XTC", "Metadata strings: titleLen=%u cap=%u, authorLen=%u cap=%u",
            static_cast<unsigned int>(m_title.size()), static_cast<unsigned int>(m_title.capacity()),
            static_cast<unsigned int>(m_author.size()), static_cast<unsigned int>(m_author.capacity()));
  }

  // Defer chapter parsing until the reader actually needs the table of contents.
  m_pageTableOffset = m_header.pageTableOffset;
  m_hasChapters = (m_header.hasChapters == 1) && (m_header.chapterOffset != 0);
  LOG_INF("XTC", "Chapter metadata deferred: available=%s", m_hasChapters ? "yes" : "no");

  m_file.close();

  // Build or reuse the on-disk page table cache before marking the parser open.
  if (!isPageTableCacheValid()) {
    LOG_INF("XTC", "Building page table cache for %u pages", m_header.pageCount);
    m_lastError = buildPageTableCache();
    if (m_lastError != XtcError::OK) {
      LOG_ERR("XTC", "Failed to build page table cache");
      return m_lastError;
    }
    const size_t heapBefore = ESP.getMaxAllocHeap();
    LOG_DBG("XTC", "Cache built, heap before defrag: free=%zu, maxAlloc=%zu", ESP.getFreeHeap(), heapBefore);

    // Defragment heap: small delay allows heap coalescing after file handles are closed
    // This typically improves MaxAlloc by 10-20KB, enabling 96KB page buffer for grayscale
    LOG_DBG("XTC", "Defragmenting heap (waiting 50ms)...");
    vTaskDelay(pdMS_TO_TICKS(50));

    const size_t heapAfter = ESP.getMaxAllocHeap();
    const size_t heapGain = heapAfter > heapBefore ? (heapAfter - heapBefore) : 0;
    if (heapGain > 0) {
      LOG_INF("XTC", "Heap defragmented: +%zu bytes contiguous (now %zu)", heapGain, heapAfter);
    } else {
      LOG_DBG("XTC", "Heap after defrag: free=%zu, maxAlloc=%zu", ESP.getFreeHeap(), heapAfter);
    }
  }

  if (!openCacheFile()) {
    LOG_ERR("XTC", "Failed to open cache file");
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Prime the sliding L2 window with the first chunk of page metadata.
  loadL2Window(0);

  LOG_DBG("XTC", "File opened, heap: free=%zu, maxAlloc=%zu", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  m_isOpen = true;
  LOG_DBG("XTC", "Opened file: %s (%u pages, cache: %s)", filepath, m_header.pageCount, m_cacheFilePath.c_str());
  return XtcError::OK;
}

void XtcParser::close() {
  closeCacheFile();

  if (m_isOpen && m_file.isOpen()) {
    m_file.close();
  }
  m_isOpen = false;
  m_l2Valid = false;
  m_l2WindowCount = 0;
  m_chaptersLoaded = false;

  for (auto& entry : m_l1Cache) {
    entry.pageIndex = 0xFFFFFFFF;
  }
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  memset(&m_header, 0, sizeof(m_header));
}

void XtcParser::ensureChaptersLoaded() {
  if (m_chaptersLoaded || !m_hasChapters) {
    return;
  }

  // Chapter parsing allocates variable-length strings, so keep it lazy.
  const XtcError err = readChapters();
  if (err != XtcError::OK) {
    LOG_ERR("XTC", "Failed to lazy-load chapters: %s", errorToString(err));
    m_hasChapters = false;
    m_chapters.clear();
    m_chapters.shrink_to_fit();
  }
  m_chaptersLoaded = true;
}

bool XtcParser::openCacheFile() {
  if (m_cacheFile.isOpen()) {
    return true;
  }
  return Storage.openFileForRead("XTC", m_cacheFilePath.c_str(), m_cacheFile);
}

void XtcParser::closeCacheFile() {
  if (m_cacheFile.isOpen()) {
    m_cacheFile.close();
  }
}

bool XtcParser::getPageInfo(uint32_t pageIndex, PageInfo& info) {
  if (pageIndex >= m_header.pageCount) {
    return false;
  }

  // L1 is the hot cache for the most recently used pages.
  if (lookupL1(pageIndex, info)) {
    LOG_DBG("XTC", "L1 hit: page %u", pageIndex);
    return true;
  }

  // L2 is the sliding window around the reader's current position.
  if (lookupL2(pageIndex, info)) {
    updateL1(pageIndex, info);
    LOG_DBG("XTC", "L2 hit: page %u", pageIndex);
    return true;
  }

  // Fall back to the SD-backed cache file, then refresh L2/L1.
  LOG_DBG("XTC", "L3 load: page %u", pageIndex);
  loadL2Window(pageIndex);

  if (lookupL2(pageIndex, info)) {
    updateL1(pageIndex, info);
    return true;
  }

  return false;
}

void XtcParser::prefetchWindow(uint32_t pageIndex) {
  if (pageIndex >= m_header.pageCount) {
    return;
  }

  // Avoid reloading the same window when the requested page is already covered.
  if (m_l2Valid && pageIndex >= m_l2WindowStart && pageIndex < m_l2WindowStart + m_l2WindowCount) {
    return;
  }

  loadL2Window(pageIndex);
}

bool XtcParser::lookupL1(uint32_t pageIndex, PageInfo& info) {
  for (const auto& entry : m_l1Cache) {
    if (entry.pageIndex == pageIndex) {
      info = entry.info;
      return true;
    }
  }
  return false;
}

void XtcParser::updateL1(uint32_t pageIndex, const PageInfo& info) {
  for (auto& entry : m_l1Cache) {
    if (entry.pageIndex == pageIndex) {
      entry.lastAccess = ++m_accessCounter;
      return;
    }
  }

  // Replace the least-recently-used entry, or fill the first empty slot.
  uint32_t oldestAccess = m_accessCounter;
  size_t oldestIndex = 0;
  bool foundEmpty = false;

  for (size_t i = 0; i < m_l1Cache.size(); i++) {
    if (m_l1Cache[i].pageIndex == 0xFFFFFFFF) {
      oldestIndex = i;
      foundEmpty = true;
      break;
    }
    if (m_l1Cache[i].lastAccess < oldestAccess) {
      oldestAccess = m_l1Cache[i].lastAccess;
      oldestIndex = i;
    }
  }

  m_l1Cache[oldestIndex].pageIndex = pageIndex;
  m_l1Cache[oldestIndex].info = info;
  m_l1Cache[oldestIndex].lastAccess = ++m_accessCounter;
}

bool XtcParser::lookupL2(uint32_t pageIndex, PageInfo& info) {
  if (!m_l2Valid) {
    return false;
  }

  if (pageIndex >= m_l2WindowStart && pageIndex < m_l2WindowStart + m_l2WindowCount) {
    size_t idx = pageIndex - m_l2WindowStart;
    info = m_l2Window[idx];
    return true;
  }
  return false;
}

void XtcParser::loadL2Window(uint32_t centerPage) {
  // Center the sliding window around the requested page when possible.
  uint32_t halfWindow = L2_WINDOW_SIZE / 2;
  uint32_t windowStart = (centerPage > halfWindow) ? centerPage - halfWindow : 0;
  uint32_t windowEnd = windowStart + L2_WINDOW_SIZE;

  if (windowEnd > m_header.pageCount) {
    windowEnd = m_header.pageCount;
    windowStart = (windowEnd > L2_WINDOW_SIZE) ? windowEnd - L2_WINDOW_SIZE : 0;
  }

  size_t windowSize = windowEnd - windowStart;
  if (windowSize == 0) {
    m_l2Valid = false;
    m_l2WindowCount = 0;
    return;
  }

  if (!m_cacheFile.isOpen() && !openCacheFile()) {
    LOG_ERR("XTC", "Cache file not available");
    m_l2Valid = false;
    m_l2WindowCount = 0;
    return;
  }

  size_t entryOffset = sizeof(PageTableCacheHeader) + windowStart * sizeof(PageInfo);
  if (!m_cacheFile.seek(entryOffset)) {
    LOG_ERR("XTC", "Failed to seek in page table cache");
    m_l2Valid = false;
    m_l2WindowCount = 0;
    return;
  }

  size_t readCount = 0;
  for (size_t i = 0; i < windowSize; i++) {
    PageInfo info;
    if (m_cacheFile.read(reinterpret_cast<uint8_t*>(&info), sizeof(PageInfo)) != sizeof(PageInfo)) {
      LOG_ERR("XTC", "Failed to read page info %zu", windowStart + i);
      break;
    }
    m_l2Window[i] = info;
    readCount++;
  }

  m_l2WindowStart = windowStart;
  m_l2WindowCount = readCount;
  m_l2Valid = (readCount > 0);

  LOG_DBG("XTC", "L2 window loaded: [%u, %u] (%zu pages)", windowStart, windowStart + readCount - 1, readCount);
}

bool XtcParser::isPageTableCacheValid() const {
  if (!Storage.exists(m_cacheFilePath.c_str())) {
    return false;
  }

  FsFile cacheFile;
  if (!Storage.openFileForRead("XTC", m_cacheFilePath.c_str(), cacheFile)) {
    return false;
  }

  uint8_t headerBuf[sizeof(PageTableCacheHeader)];
  if (cacheFile.read(headerBuf, sizeof(headerBuf)) != sizeof(headerBuf)) {
    cacheFile.close();
    return false;
  }

  PageTableCacheHeader header;
  safeDeserializeHeader(headerBuf, header);

  if (header.magic != PAGE_TABLE_CACHE_MAGIC || header.version != PAGE_TABLE_CACHE_VERSION) {
    cacheFile.close();
    return false;
  }

  // The cache must match both the page count and the original file size.
  if (header.pageCount != m_header.pageCount) {
    cacheFile.close();
    return false;
  }

  uint32_t expectedSize = sizeof(PageTableCacheHeader) + header.pageCount * sizeof(PageInfo);
  if (cacheFile.size() < expectedSize) {
    cacheFile.close();
    return false;
  }

  if (header.originalSize > 0) {
    FsFile originalFile;
    if (Storage.openFileForRead("XTC", m_originalPath.c_str(), originalFile)) {
      uint64_t currentSize = originalFile.size();
      originalFile.close();
      if (currentSize != header.originalSize) {
        LOG_INF("XTC", "Cache invalidated: file size changed");
        cacheFile.close();
        return false;
      }
    }
  }

  cacheFile.close();
  return true;
}

XtcError XtcParser::buildPageTableCache() {
  FsFile originalFile;
  if (!Storage.openFileForRead("XTC", m_originalPath.c_str(), originalFile)) {
    return XtcError::FILE_NOT_FOUND;
  }

  size_t lastSlash = m_cacheFilePath.find_last_of('/');
  if (lastSlash != std::string::npos) {
    std::string cacheDir = m_cacheFilePath.substr(0, lastSlash);
    Storage.mkdir(cacheDir.c_str());
  }

  FsFile cacheFile;
  if (!Storage.openFileForWrite("XTC", m_cacheFilePath.c_str(), cacheFile)) {
    originalFile.close();
    return XtcError::WRITE_ERROR;
  }

  // Persist a compact PageInfo array so we do not need to hold the full table in RAM.
  PageTableCacheHeader header;
  header.magic = PAGE_TABLE_CACHE_MAGIC;
  header.version = PAGE_TABLE_CACHE_VERSION;
  header.pageCount = m_header.pageCount;
  header.originalHash = calculateFileHash(m_originalPath.c_str());
  header.originalSize = originalFile.size();
  header.entrySize = sizeof(PageInfo);
  header.reserved = 0;

  uint8_t headerBuf[sizeof(PageTableCacheHeader)];
  safeSerializeHeader(headerBuf, header);
  if (cacheFile.write(headerBuf, sizeof(headerBuf)) != sizeof(headerBuf)) {
    cacheFile.close();
    originalFile.close();
    return XtcError::WRITE_ERROR;
  }

  if (!seekToOffset(originalFile, m_pageTableOffset)) {
    cacheFile.close();
    originalFile.close();
    return XtcError::READ_ERROR;
  }

  // Convert the source page table entries into the cached PageInfo layout.
  for (uint16_t i = 0; i < m_header.pageCount; i++) {
    PageTableEntry entry;
    if (originalFile.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry)) != sizeof(PageTableEntry)) {
      LOG_ERR("XTC", "Failed to read page table entry %u", i);
      cacheFile.close();
      originalFile.close();
      return XtcError::READ_ERROR;
    }

    PageInfo info;
    info.offset = entry.dataOffset;
    info.size = entry.dataSize;
    info.width = entry.width;
    info.height = entry.height;
    info.bitDepth = m_bitDepth;
    info.padding = 0;

    if (cacheFile.write(reinterpret_cast<const uint8_t*>(&info), sizeof(info)) != sizeof(info)) {
      cacheFile.close();
      originalFile.close();
      return XtcError::WRITE_ERROR;
    }
  }

  cacheFile.close();
  originalFile.close();

  LOG_INF("XTC", "Page table cache built: %u entries", m_header.pageCount);
  return XtcError::OK;
}

uint32_t XtcParser::calculateFileHash(const char* filepath) const {
  uint32_t hash = 0;
  size_t len = strlen(filepath);

  for (size_t i = 0; i < len; i++) {
    hash = hash * 31 + static_cast<uint8_t>(filepath[i]);
  }

  FsFile file;
  if (Storage.openFileForRead("XTC", filepath, file)) {
    uint64_t size = file.size();
    hash ^= static_cast<uint32_t>(size);
    hash ^= static_cast<uint32_t>(size >> 32);
    file.close();
  }

  return hash;
}

XtcError XtcParser::readHeader() {
  // Read the fixed-size XTC header first.
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader));
  if (bytesRead != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }

  // Verify magic number (accept both XTC and XTCH)
  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) {
    LOG_DBG("XTC", "Invalid magic: 0x%08X (expected 0x%08X or 0x%08X)", m_header.magic, XTC_MAGIC, XTCH_MAGIC);
    return XtcError::INVALID_MAGIC;
  }

  // Determine bit depth from file magic
  m_bitDepth = (m_header.magic == XTCH_MAGIC) ? 2 : 1;

  // Check version
  // Currently, version 1.0 is the only valid version, however some generators are swapping the bytes around, so we
  // accept both 1.0 and 0.1 for compatibility
  const bool validVersion = m_header.versionMajor == 1 && m_header.versionMinor == 0 ||
                            m_header.versionMajor == 0 && m_header.versionMinor == 1;
  if (!validVersion) {
    LOG_DBG("XTC", "Unsupported version: %u.%u", m_header.versionMajor, m_header.versionMinor);
    return XtcError::INVALID_VERSION;
  }

  // Basic validation
  if (m_header.pageCount == 0) {
    return XtcError::CORRUPTED_HEADER;
  }

  LOG_DBG("XTC", "Header: magic=0x%08X (%s), ver=%u.%u, pages=%u, bitDepth=%u", m_header.magic,
          (m_header.magic == XTCH_MAGIC) ? "XTCH" : "XTC", m_header.versionMajor, m_header.versionMinor,
          m_header.pageCount, m_bitDepth);

  return XtcError::OK;
}

XtcError XtcParser::readTitle() {
  constexpr auto titleOffset = 0x38;
  if (!m_file.seek(titleOffset)) {
    return XtcError::READ_ERROR;
  }

  char titleBuf[128] = {0};
  m_file.read(titleBuf, sizeof(titleBuf) - 1);
  m_title = titleBuf;

  LOG_DBG("XTC", "Title: %s", m_title.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readAuthor() {
  // Read author as null-terminated UTF-8 string with max length 64, directly following title
  constexpr auto authorOffset = 0xB8;
  if (!m_file.seek(authorOffset)) {
    return XtcError::READ_ERROR;
  }

  char authorBuf[64] = {0};
  m_file.read(authorBuf, sizeof(authorBuf) - 1);
  m_author = authorBuf;

  LOG_DBG("XTC", "Author: %s", m_author.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readChapters() {
  m_hasChapters = false;
  m_chapters.clear();
  m_chapters.shrink_to_fit();

  // Reopen the original file on demand because open() closes it after cache initialization.
  if (!m_file.isOpen()) {
    if (!Storage.openFileForRead("XTC", m_originalPath.c_str(), m_file)) {
      return XtcError::FILE_NOT_FOUND;
    }
  }

  uint8_t hasChaptersFlag = 0;
  if (!m_file.seek(0x0B)) {
    return XtcError::READ_ERROR;
  }
  if (m_file.read(&hasChaptersFlag, sizeof(hasChaptersFlag)) != sizeof(hasChaptersFlag)) {
    return XtcError::READ_ERROR;
  }

  if (hasChaptersFlag != 1) {
    return XtcError::OK;
  }

  uint64_t chapterOffset = 0;
  if (!m_file.seek(0x30)) {
    return XtcError::READ_ERROR;
  }
  if (m_file.read(reinterpret_cast<uint8_t*>(&chapterOffset), sizeof(chapterOffset)) != sizeof(chapterOffset)) {
    return XtcError::READ_ERROR;
  }

  if (chapterOffset == 0) {
    return XtcError::OK;
  }

  const uint64_t fileSize = m_file.size();
  constexpr size_t chapterSize = 96;

  if (chapterOffset < sizeof(XtcHeader) || chapterOffset >= fileSize) {
    return XtcError::OK;
  }

  if (fileSize - chapterOffset < chapterSize) {
    return XtcError::OK;
  }

  uint64_t maxOffset = 0;
  if (m_header.pageTableOffset > chapterOffset) {
    maxOffset = m_header.pageTableOffset;
  } else if (m_header.dataOffset > chapterOffset) {
    maxOffset = m_header.dataOffset;
  } else {
    maxOffset = fileSize;
  }

  if (maxOffset <= chapterOffset) {
    return XtcError::OK;
  }

  const uint64_t available = maxOffset - chapterOffset;
  const uint64_t chapterCount64 = available / chapterSize;
  if (chapterCount64 == 0) {
    return XtcError::OK;
  }

  if (chapterCount64 > MAX_CHAPTERS || chapterCount64 > std::numeric_limits<size_t>::max()) {
    LOG_ERR("XTC", "Chapter table too large: available=%llu chapterCount=%llu",
            static_cast<unsigned long long>(available), static_cast<unsigned long long>(chapterCount64));
    return XtcError::CORRUPTED_HEADER;
  }

  const size_t chapterCount = static_cast<size_t>(chapterCount64);
  if (chapterCount == 0) {
    return XtcError::OK;
  }

  const size_t freeHeapBefore = ESP.getFreeHeap();
  const size_t maxAllocBefore = ESP.getMaxAllocHeap();

  if (!seekToOffset(m_file, chapterOffset)) {
    return XtcError::READ_ERROR;
  }

  std::vector<uint8_t> chapterBuf(chapterSize);
  m_chapters.reserve(chapterCount);
  for (size_t i = 0; i < chapterCount; i++) {
    if (m_file.read(chapterBuf.data(), chapterSize) != chapterSize) {
      return XtcError::READ_ERROR;
    }

    char nameBuf[81];
    memcpy(nameBuf, chapterBuf.data(), 80);
    nameBuf[80] = '\0';
    const size_t nameLen = strnlen(nameBuf, 80);
    std::string name(nameBuf, nameLen);

    uint16_t startPage = 0;
    uint16_t endPage = 0;
    memcpy(&startPage, chapterBuf.data() + 0x50, sizeof(startPage));
    memcpy(&endPage, chapterBuf.data() + 0x52, sizeof(endPage));

    if (name.empty() && startPage == 0 && endPage == 0) {
      break;
    }

    if (startPage > 0) {
      startPage--;
    }
    if (endPage > 0) {
      endPage--;
    }

    if (startPage >= m_header.pageCount) {
      continue;
    }

    if (endPage >= m_header.pageCount) {
      endPage = m_header.pageCount - 1;
    }

    if (startPage > endPage) {
      continue;
    }

    ChapterInfo chapter{std::move(name), startPage, endPage};
    m_chapters.push_back(std::move(chapter));
  }

  m_chapters.shrink_to_fit();
  m_hasChapters = !m_chapters.empty();
  size_t chapterNameBytes = 0;
  for (const auto& chapter : m_chapters) {
    chapterNameBytes += chapter.name.capacity() + 1;
  }
  const size_t chapterVectorBytes = m_chapters.capacity() * sizeof(ChapterInfo);
  const size_t totalChapterBytes = chapterVectorBytes + chapterNameBytes;
  const size_t freeHeapAfter = ESP.getFreeHeap();
  const size_t maxAllocAfter = ESP.getMaxAllocHeap();
  const int heapDelta = static_cast<int>(freeHeapBefore) - static_cast<int>(freeHeapAfter);
  const int maxAllocDelta = static_cast<int>(maxAllocBefore) - static_cast<int>(maxAllocAfter);
  LOG_INF("XTC", "Chapter metadata: count=%u, vector~=%zu, names~=%zu, total~=%zu, heapDelta=%d, maxAllocDelta=%d",
          static_cast<unsigned int>(m_chapters.size()), chapterVectorBytes, chapterNameBytes, totalChapterBytes,
          heapDelta, maxAllocDelta);
  return XtcError::OK;
}

const std::vector<ChapterInfo>& XtcParser::getChapters() {
  ensureChaptersLoaded();
  return m_chapters;
}

size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (pageIndex >= m_header.pageCount) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return 0;
  }

  // Resolve the page location through the cache hierarchy before touching the data file.
  PageInfo info;
  if (!getPageInfo(pageIndex, info)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  // Reopen the source file lazily because normal parser open() does not keep it pinned.
  if (!m_file.isOpen()) {
    if (!Storage.openFileForRead("XTC", m_originalPath.c_str(), m_file)) {
      m_lastError = XtcError::FILE_NOT_FOUND;
      return 0;
    }
  }

  if (!seekToOffset(m_file, info.offset)) {
    LOG_DBG("XTC", "Failed to seek to page %u at offset %llu", pageIndex, static_cast<unsigned long long>(info.offset));
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  // Read page header (XTG for 1-bit, XTH for 2-bit - same structure)
  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  if (headerRead != sizeof(XtgPageHeader)) {
    LOG_DBG("XTC", "Failed to read page header for page %u", pageIndex);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  // Verify page magic (XTG for 1-bit, XTH for 2-bit)
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) {
    LOG_DBG("XTC", "Invalid page magic for page %u: 0x%08X (expected 0x%08X)", pageIndex, pageHeader.magic,
            expectedMagic);
    m_lastError = XtcError::INVALID_MAGIC;
    return 0;
  }

  // Calculate bitmap size based on bit depth
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t bitmapSize;
  if (m_bitDepth == 2) {
    // XTH: two bit planes, each containing (width * height) bits rounded up to bytes
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  // The caller owns the buffer, so fail early if it is too small.
  if (bufferSize < bitmapSize) {
    LOG_DBG("XTC", "Buffer too small: need %u, have %u", bitmapSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  // Read the bitmap payload into the caller-provided buffer.
  size_t bytesRead = m_file.read(buffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    LOG_DBG("XTC", "Page read error: expected %u, got %u", bitmapSize, bytesRead);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  m_lastError = XtcError::OK;
  return bytesRead;
}

XtcError XtcParser::loadPageStreaming(uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      size_t chunkSize) {
  if (!m_isOpen) {
    return XtcError::FILE_NOT_FOUND;
  }

  if (pageIndex >= m_header.pageCount) {
    return XtcError::PAGE_OUT_OF_RANGE;
  }

  // Streaming uses the same cache lookup path but reads the payload in chunks.
  PageInfo info;
  if (!getPageInfo(pageIndex, info)) {
    return XtcError::READ_ERROR;
  }

  // Reopen the source file on demand for streaming reads as well.
  if (!m_file.isOpen()) {
    if (!Storage.openFileForRead("XTC", m_originalPath.c_str(), m_file)) {
      return XtcError::FILE_NOT_FOUND;
    }
  }

  if (!seekToOffset(m_file, info.offset)) {
    LOG_DBG("XTC", "Failed to seek to page %u at offset %llu", pageIndex, static_cast<unsigned long long>(info.offset));
    return XtcError::READ_ERROR;
  }

  // Read and validate the page header before yielding any bitmap bytes.
  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (headerRead != sizeof(XtgPageHeader) || pageHeader.magic != expectedMagic) {
    return XtcError::READ_ERROR;
  }

  // Calculate bitmap size based on bit depth
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, ((width * height + 7) / 8) * 2 bytes
  // Match the bitmap sizing rules used by the non-streaming path.
  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  // Feed the bitmap to the callback in bounded chunks to keep peak memory low.
  std::vector<uint8_t> chunk(chunkSize);
  size_t totalRead = 0;

  while (totalRead < bitmapSize) {
    size_t toRead = std::min(chunkSize, bitmapSize - totalRead);
    size_t bytesRead = m_file.read(chunk.data(), toRead);

    if (bytesRead == 0) {
      return XtcError::READ_ERROR;
    }

    callback(chunk.data(), bytesRead, totalRead);
    totalRead += bytesRead;
  }

  return XtcError::OK;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  FsFile file;
  if (!Storage.openFileForRead("XTC", filepath, file)) {
    return false;
  }

  uint32_t magic = 0;
  size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  file.close();

  if (bytesRead != sizeof(magic)) {
    return false;
  }

  return (magic == XTC_MAGIC || magic == XTCH_MAGIC);
}

}  // namespace xtc
