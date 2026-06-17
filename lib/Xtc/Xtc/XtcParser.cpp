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
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace xtc {

namespace {

bool calculateBitmapSize(uint16_t width, uint16_t height, uint8_t bitDepth, uint32_t& out) {
  uint64_t size = 0;
  if (bitDepth == 2) {
    const uint64_t colBytes = (static_cast<uint64_t>(height) + 7) / 8;
    size = static_cast<uint64_t>(width) * colBytes * 2;
  } else {
    const uint64_t rowBytes = (static_cast<uint64_t>(width) + 7) / 8;
    size = rowBytes * height;
  }
  if (size == 0 || size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  out = static_cast<uint32_t>(size);
  return true;
}

}  // namespace

XtcParser::XtcParser()
    : m_isOpen(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_chaptersLoaded(false),
      m_lastError(XtcError::OK) {
  memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::open(const char* filepath) {
  // Close if already open
  if (m_isOpen) {
    close();
  }

  m_filepath = filepath;

  // Open file
  if (!Storage.openFileForRead("XTC", filepath, m_file)) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Read header
  m_lastError = readHeader();
  if (m_lastError != XtcError::OK) {
    LOG_DBG("XTC", "Failed to read header: %s", errorToString(m_lastError));
    // Explicit close() required: member variable persists beyond function scope
    m_file.close();
    return m_lastError;
  }

  // Read title & author if available
  if (m_header.hasMetadata) {
    m_lastError = readTitle();
    if (m_lastError != XtcError::OK) {
      LOG_DBG("XTC", "Failed to read title: %s", errorToString(m_lastError));
      // Explicit close() required: member variable persists beyond function scope
      m_file.close();
      return m_lastError;
    }
    m_lastError = readAuthor();
    if (m_lastError != XtcError::OK) {
      LOG_DBG("XTC", "Failed to read author: %s", errorToString(m_lastError));
      // Explicit close() required: member variable persists beyond function scope
      m_file.close();
      return m_lastError;
    }
    // Trim excess capacity from metadata strings
    m_title.shrink_to_fit();
    m_author.shrink_to_fit();
  }

  // Read first page info for default dimensions (no bulk page table allocation)
  m_lastError = readFirstPageInfo();
  if (m_lastError != XtcError::OK) {
    LOG_DBG("XTC", "Failed to read first page info: %s", errorToString(m_lastError));
    // Explicit close() required: member variable persists beyond function scope
    m_file.close();
    return m_lastError;
  }

  // Defer chapter parsing until actually needed (lazy load).
  // Chapter strings can use significant heap; keeping them out of memory
  // during rendering leaves more room for the page bitmap buffer.
  // Older XTC files start the page table at 0x30, so they do not have the later
  // chapterOffset field even if the bytes read into that slot are non-zero.
  m_hasChapters = (m_header.hasChapters == 1 && m_header.pageTableOffset >= sizeof(XtcHeader));
  m_chaptersLoaded = false;

  // Close the source file to free its internal SdFat buffers.
  // It will be reopened on-demand for page table lookups and bitmap reads.
  m_file.close();

  m_isOpen = true;
  LOG_DBG("XTC", "Opened file: %s (%u pages, %dx%d)", filepath, m_header.pageCount, m_defaultWidth, m_defaultHeight);
  return XtcError::OK;
}

void XtcParser::close() {
  closeFile();
  m_isOpen = false;
  m_chaptersLoaded = false;
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  m_hasChapters = false;
  memset(&m_header, 0, sizeof(m_header));
}

bool XtcParser::ensureFileOpen() {
  if (m_file.isOpen()) {
    return true;
  }
  return Storage.openFileForRead("XTC", m_filepath.c_str(), m_file);
}

void XtcParser::closeFile() {
  if (m_file.isOpen()) {
    m_file.close();
  }
}

XtcError XtcParser::readHeader() {
  // Read first 56 bytes of header
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

XtcError XtcParser::readFirstPageInfo() {
  if (m_header.pageTableOffset == 0) {
    LOG_DBG("XTC", "Page table offset is 0, cannot read");
    return XtcError::CORRUPTED_HEADER;
  }

  // Verify the file is large enough to contain the full page table
  const uint64_t fileSize = m_file.fileSize64();
  const uint64_t pageTableSize = static_cast<uint64_t>(m_header.pageCount) * sizeof(PageTableEntry);
  if (m_header.pageTableOffset < XTC_LEGACY_HEADER_SIZE || m_header.pageTableOffset > fileSize ||
      pageTableSize > fileSize - m_header.pageTableOffset) {
    LOG_DBG("XTC",
            "Page table exceeds file bounds: file=%llu tableOffset=%llu tableSize=%llu pages=%u entrySize=%u "
            "dataOffset=%llu minTableOffset=%llu",
            static_cast<unsigned long long>(fileSize), static_cast<unsigned long long>(m_header.pageTableOffset),
            static_cast<unsigned long long>(pageTableSize), m_header.pageCount,
            static_cast<unsigned int>(sizeof(PageTableEntry)), static_cast<unsigned long long>(m_header.dataOffset),
            static_cast<unsigned long long>(XTC_LEGACY_HEADER_SIZE));
    return XtcError::CORRUPTED_HEADER;
  }

  // Read only the first entry to get default page dimensions
  // All other entries are read on-demand via readPageTableEntry()
  // This avoids allocating pageCount * 16 bytes (e.g. 65KB for 4000+ pages)
  PageTableEntry entry;
  if (!m_file.seek64(m_header.pageTableOffset)) {
    LOG_DBG("XTC", "Failed to seek to page table at %llu", m_header.pageTableOffset);
    return XtcError::READ_ERROR;
  }
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
  if (bytesRead != sizeof(PageTableEntry)) {
    LOG_DBG("XTC", "Failed to read first page table entry");
    return XtcError::READ_ERROR;
  }

  m_defaultWidth = entry.width;
  m_defaultHeight = entry.height;

  LOG_DBG("XTC", "Page table validated: %u pages, default %dx%d", m_header.pageCount, m_defaultWidth, m_defaultHeight);
  return XtcError::OK;
}

bool XtcParser::readPageTableEntry(uint32_t pageIndex, PageInfo& info) {
  if (pageIndex >= m_header.pageCount) {
    return false;
  }

  if (!ensureFileOpen()) {
    LOG_DBG("XTC", "Failed to reopen file for page table read");
    return false;
  }

  // Seek to the specific page table entry on the SD card
  const uint64_t entryOffset = m_header.pageTableOffset + static_cast<uint64_t>(pageIndex) * sizeof(PageTableEntry);
  if (!m_file.seek64(entryOffset)) {
    LOG_DBG("XTC", "Failed to seek to page table entry %lu at %llu", pageIndex, entryOffset);
    return false;
  }

  PageTableEntry entry;
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
  if (bytesRead != sizeof(PageTableEntry)) {
    LOG_DBG("XTC", "Failed to read page table entry %lu", pageIndex);
    return false;
  }

  info.offset = entry.dataOffset;
  info.size = entry.dataSize;
  info.width = entry.width;
  info.height = entry.height;
  info.bitDepth = m_bitDepth;
  return true;
}

XtcError XtcParser::readChapters() {
  m_chapters.clear();

  if (!ensureFileOpen()) {
    return XtcError::READ_ERROR;
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

  const uint64_t fileSize = m_file.fileSize64();
  if (chapterOffset < sizeof(XtcHeader) || chapterOffset >= fileSize || chapterOffset + 96 > fileSize) {
    return XtcError::OK;
  }

  // Clamp maxOffset to fileSize so bogus header values can't inflate chapterCount
  uint64_t maxOffset = fileSize;
  if (m_header.pageTableOffset > chapterOffset && m_header.pageTableOffset <= fileSize) {
    maxOffset = m_header.pageTableOffset;
  } else if (m_header.dataOffset > chapterOffset && m_header.dataOffset <= fileSize) {
    maxOffset = m_header.dataOffset;
  }

  if (maxOffset <= chapterOffset) {
    return XtcError::OK;
  }

  constexpr size_t chapterSize = 96;
  const uint64_t available = maxOffset - chapterOffset;
  const size_t chapterCount = static_cast<size_t>(available / chapterSize);
  if (chapterCount == 0) {
    return XtcError::OK;
  }

  if (!m_file.seek64(chapterOffset)) {
    return XtcError::READ_ERROR;
  }

  m_chapters.reserve(chapterCount);
  std::vector<uint8_t> chapterBuf(chapterSize);
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

  m_hasChapters = !m_chapters.empty();
  LOG_DBG("XTC", "Chapters: %u", static_cast<unsigned int>(m_chapters.size()));
  return XtcError::OK;
}

const std::vector<ChapterInfo>& XtcParser::getChapters() {
  // Lazy load chapters on first access
  if (!m_chaptersLoaded && m_hasChapters) {
    const XtcError err = readChapters();
    if (err != XtcError::OK) {
      LOG_ERR("XTC", "Failed to lazy-load chapters: %s", errorToString(err));
      m_hasChapters = false;
      m_chapters.clear();
    }
    m_chaptersLoaded = true;
    // Close file after chapter read to free buffers for rendering
    closeFile();
  }
  return m_chapters;
}

bool XtcParser::getPageInfo(uint32_t pageIndex, PageInfo& info) { return readPageTableEntry(pageIndex, info); }

XtcError XtcParser::beginPageBitmapRead(uint32_t pageIndex, PageBitmapLayout& layout) {
  memset(&layout, 0, sizeof(layout));

  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  if (pageIndex >= m_header.pageCount) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return m_lastError;
  }

  PageInfo page;
  if (!readPageTableEntry(pageIndex, page)) {
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }

  if (!ensureFileOpen()) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  if (!m_file.seek64(page.offset)) {
    LOG_DBG("XTC", "Failed to seek to page %u at offset %llu", pageIndex, static_cast<unsigned long long>(page.offset));
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  if (headerRead != sizeof(XtgPageHeader)) {
    LOG_DBG("XTC", "Failed to read page header for page %u", pageIndex);
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }

  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) {
    LOG_DBG("XTC", "Invalid page magic for page %u: 0x%08X (expected 0x%08X)", pageIndex, pageHeader.magic,
            expectedMagic);
    m_lastError = XtcError::INVALID_MAGIC;
    return m_lastError;
  }

  uint32_t bitmapSize = 0;
  if (!calculateBitmapSize(pageHeader.width, pageHeader.height, m_bitDepth, bitmapSize)) {
    LOG_DBG("XTC", "Invalid bitmap dimensions for page %u: %ux%u", pageIndex, pageHeader.width, pageHeader.height);
    m_lastError = XtcError::CORRUPTED_HEADER;
    return m_lastError;
  }

  if (pageHeader.dataSize != 0 && pageHeader.dataSize < bitmapSize) {
    LOG_DBG("XTC", "Page %u bitmap smaller than layout: header=%u expected=%u", pageIndex, pageHeader.dataSize,
            bitmapSize);
    m_lastError = XtcError::CORRUPTED_HEADER;
    return m_lastError;
  }

  if (page.size != 0 && page.size < bitmapSize) {
    LOG_DBG("XTC", "Page %u table entry smaller than layout: entry=%u expected=%u", pageIndex, page.size,
            bitmapSize);
    m_lastError = XtcError::CORRUPTED_HEADER;
    return m_lastError;
  }

  const uint64_t bitmapOffset = page.offset + sizeof(XtgPageHeader);
  const uint64_t fileSize = m_file.fileSize64();
  if (bitmapOffset > fileSize || static_cast<uint64_t>(bitmapSize) > fileSize - bitmapOffset) {
    LOG_DBG("XTC", "Page %u bitmap exceeds file bounds", pageIndex);
    m_lastError = XtcError::CORRUPTED_HEADER;
    return m_lastError;
  }

  layout.bitmapOffset = bitmapOffset;
  layout.bitmapSize = bitmapSize;
  layout.width = pageHeader.width;
  layout.height = pageHeader.height;
  layout.bitDepth = m_bitDepth;
  layout.colorMode = pageHeader.colorMode;
  layout.compression = pageHeader.compression;
  layout.pageMagic = pageHeader.magic;
  layout.headerDataSize = pageHeader.dataSize;

  m_lastError = XtcError::OK;
  return m_lastError;
}

XtcError XtcParser::readPageBitmapRange(const PageBitmapLayout& layout, uint32_t relativeOffset, uint8_t* dst,
                                        size_t len) {
  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }
  if (!dst && len > 0) {
    m_lastError = XtcError::MEMORY_ERROR;
    return m_lastError;
  }
  if (static_cast<uint64_t>(relativeOffset) + len > layout.bitmapSize) {
    LOG_DBG("XTC", "Bitmap range exceeds page: offset=%u len=%u size=%u", relativeOffset, static_cast<unsigned>(len),
            layout.bitmapSize);
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }
  if (!ensureFileOpen()) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }
  if (!m_file.seek64(layout.bitmapOffset + relativeOffset)) {
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }

  const size_t bytesRead = m_file.read(dst, len);
  if (bytesRead != len) {
    LOG_DBG("XTC", "Bitmap range read error: expected %u, got %u", static_cast<unsigned>(len),
            static_cast<unsigned>(bytesRead));
    m_lastError = XtcError::READ_ERROR;
    return m_lastError;
  }

  m_lastError = XtcError::OK;
  return m_lastError;
}

void XtcParser::endPageBitmapRead() { closeFile(); }

size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  PageBitmapLayout layout;
  if (beginPageBitmapRead(pageIndex, layout) != XtcError::OK) {
    endPageBitmapRead();
    return 0;
  }

  if (bufferSize < layout.bitmapSize) {
    LOG_DBG("XTC", "Buffer too small: need %u, have %u", layout.bitmapSize, static_cast<unsigned>(bufferSize));
    m_lastError = XtcError::MEMORY_ERROR;
    endPageBitmapRead();
    return 0;
  }

  if (readPageBitmapRange(layout, 0, buffer, layout.bitmapSize) != XtcError::OK) {
    endPageBitmapRead();
    return 0;
  }

  endPageBitmapRead();
  m_lastError = XtcError::OK;
  return layout.bitmapSize;
}

XtcError XtcParser::loadPageStreaming(uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      size_t chunkSize) {
  if (chunkSize == 0) return XtcError::MEMORY_ERROR;

  PageBitmapLayout layout;
  XtcError err = beginPageBitmapRead(pageIndex, layout);
  if (err != XtcError::OK) {
    endPageBitmapRead();
    return err;
  }

  auto chunk = makeUniqueNoThrow<uint8_t[]>(chunkSize);
  if (!chunk) {
    m_lastError = XtcError::MEMORY_ERROR;
    endPageBitmapRead();
    return m_lastError;
  }
  size_t totalRead = 0;

  while (totalRead < layout.bitmapSize) {
    size_t toRead = std::min(chunkSize, static_cast<size_t>(layout.bitmapSize) - totalRead);
    err = readPageBitmapRange(layout, totalRead, chunk.get(), toRead);
    if (err != XtcError::OK) {
      endPageBitmapRead();
      return err;
    }

    callback(chunk.get(), toRead, totalRead);
    totalRead += toRead;
  }

  endPageBitmapRead();
  m_lastError = XtcError::OK;
  return XtcError::OK;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  HalFile file;
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
