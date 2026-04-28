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

#include <cstring>

namespace xtc {

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
  m_hasChapters = (m_header.hasChapters == 1);
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
  const uint64_t fileSize = m_file.size();
  const uint64_t pageTableSize = static_cast<uint64_t>(m_header.pageCount) * sizeof(PageTableEntry);
  if (m_header.pageTableOffset < sizeof(XtcHeader) || m_header.pageTableOffset > fileSize ||
      pageTableSize > fileSize - m_header.pageTableOffset) {
    LOG_DBG("XTC", "Page table exceeds file bounds");
    return XtcError::CORRUPTED_HEADER;
  }

  // Read only the first entry to get default page dimensions
  // All other entries are read on-demand via readPageTableEntry()
  // This avoids allocating pageCount * 16 bytes (e.g. 65KB for 4000+ pages)
  PageTableEntry entry;
  if (!m_file.seek(m_header.pageTableOffset)) {
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
  if (!m_file.seek(entryOffset)) {
    LOG_DBG("XTC", "Failed to seek to page table entry %lu at %llu", pageIndex, entryOffset);
    return false;
  }

  PageTableEntry entry;
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
  if (bytesRead != sizeof(PageTableEntry)) {
    LOG_DBG("XTC", "Failed to read page table entry %lu", pageIndex);
    return false;
  }

  info.offset = static_cast<uint32_t>(entry.dataOffset);
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

  const uint64_t fileSize = m_file.size();
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

  if (!m_file.seek(chapterOffset)) {
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

size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (pageIndex >= m_header.pageCount) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return 0;
  }

  PageInfo page;
  if (!readPageTableEntry(pageIndex, page)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  if (!ensureFileOpen()) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  // Seek to page data
  if (!m_file.seek(page.offset)) {
    LOG_DBG("XTC", "Failed to seek to page %u at offset %lu", pageIndex, page.offset);
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

  // Check buffer size
  if (bufferSize < bitmapSize) {
    LOG_DBG("XTC", "Buffer too small: need %u, have %u", bitmapSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  // Read bitmap data
  size_t bytesRead = m_file.read(buffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    LOG_DBG("XTC", "Page read error: expected %u, got %u", bitmapSize, bytesRead);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  m_lastError = XtcError::OK;
  return bytesRead;
}

size_t XtcParser::loadPageMsb(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (m_bitDepth != 2) {
    return loadPage(pageIndex, buffer, bufferSize);
  }

  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (pageIndex >= m_header.pageCount) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return 0;
  }

  PageInfo page;
  if (!readPageTableEntry(pageIndex, page)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  if (!ensureFileOpen()) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (!m_file.seek(page.offset)) {
    LOG_DBG("XTC", "Failed to seek to page %u at offset %lu", pageIndex, page.offset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  if (headerRead != sizeof(XtgPageHeader)) {
    LOG_DBG("XTC", "Failed to read page header for page %u", pageIndex);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  if (pageHeader.magic != XTH_MAGIC) {
    LOG_DBG("XTC", "Invalid page magic for page %u: 0x%08X", pageIndex, pageHeader.magic);
    m_lastError = XtcError::INVALID_MAGIC;
    return 0;
  }

  // Only plane1 (MSB): (width * height + 7) / 8 bytes
  const size_t planeSize = (static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8;

  if (bufferSize < planeSize) {
    LOG_DBG("XTC", "Buffer too small for MSB plane: need %u, have %u", planeSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  size_t bytesRead = m_file.read(buffer, planeSize);
  if (bytesRead != planeSize) {
    LOG_DBG("XTC", "MSB plane read error: expected %u, got %u", planeSize, bytesRead);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  m_lastError = XtcError::OK;
  return bytesRead;
}

size_t XtcParser::loadPageLsb(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (m_bitDepth != 2) return loadPage(pageIndex, buffer, bufferSize);

  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }
  if (pageIndex >= m_header.pageCount) {
    m_lastError = XtcError::PAGE_OUT_OF_RANGE;
    return 0;
  }

  PageInfo page;
  if (!readPageTableEntry(pageIndex, page)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  if (!ensureFileOpen()) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (!m_file.seek(page.offset)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  XtgPageHeader pageHeader;
  if (m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader)) != sizeof(XtgPageHeader)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }
  if (pageHeader.magic != XTH_MAGIC) {
    LOG_DBG("XTC", "loadPageLsb: invalid magic for page %u: 0x%08X", pageIndex, pageHeader.magic);
    m_lastError = XtcError::INVALID_MAGIC;
    return 0;
  }

  const size_t planeSize = (static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8;
  const uint32_t plane2Offset = page.offset + sizeof(XtgPageHeader) + static_cast<uint32_t>(planeSize);

  if (!m_file.seek(plane2Offset)) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  if (bufferSize < planeSize) {
    LOG_DBG("XTC", "Buffer too small for LSB plane: need %u, have %u", planeSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  size_t bytesRead = m_file.read(buffer, planeSize);
  if (bytesRead != planeSize) {
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

  PageInfo page;
  if (!readPageTableEntry(pageIndex, page)) {
    return XtcError::READ_ERROR;
  }

  if (!ensureFileOpen()) {
    return XtcError::FILE_NOT_FOUND;
  }

  // Seek to page data
  if (!m_file.seek(page.offset)) {
    return XtcError::READ_ERROR;
  }

  // Read and skip page header (XTG for 1-bit, XTH for 2-bit)
  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (headerRead != sizeof(XtgPageHeader) || pageHeader.magic != expectedMagic) {
    return XtcError::READ_ERROR;
  }

  // Calculate bitmap size based on bit depth
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, ((width * height + 7) / 8) * 2 bytes
  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  // Read in chunks
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
