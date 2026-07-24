/**
 * XtcParser.cpp
 *
 * Strict reader for the CrossVi-supported XTC/XTCH v1.0 subset.
 */

#include "XtcParser.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "XtcPageLayout.h"

namespace xtc {
namespace {

constexpr size_t IDENTITY_CHUNK_SIZE = 2048;
constexpr size_t IDENTITY_YIELD_BYTES = 64U * 1024U;
constexpr uint64_t FNV64_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV64_PRIME = 1099511628211ULL;

bool rangeWithin(const uint64_t offset, const uint64_t length, const uint64_t fileSize) {
  return offset <= fileSize && length <= fileSize - offset;
}

void updateRawIdentity(const uint8_t* data, const size_t length, uint32_t& crc, uint64_t& fnv) {
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    fnv ^= data[index];
    fnv *= FNV64_PRIME;
  }
}

bool hasTerminator(const uint8_t* bytes, const size_t length) { return std::memchr(bytes, 0, length) != nullptr; }

}  // namespace

XtcParser::XtcParser()
    : m_isOpen(false),
      m_fileSize(0),
      m_chapterCount(0),
      m_hasSourceIdentity(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_chaptersLoaded(false),
      m_lastError(XtcError::OK) {
  std::memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::failOpen(const XtcError error) {
  closeFile();
  m_isOpen = false;
  m_fileSize = 0;
  m_chapterCount = 0;
  m_hasSourceIdentity = false;
  m_sourceIdentity = {};
  m_chaptersLoaded = false;
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  m_hasChapters = false;
  std::memset(&m_header, 0, sizeof(m_header));
  m_lastError = error;
  return error;
}

XtcError XtcParser::open(const char* filepath) {
  close();
  m_lastError = XtcError::OK;
  if (!filepath || filepath[0] == '\0') return failOpen(XtcError::INVALID_ARGUMENT);
  m_filepath = filepath;

  if (!Storage.openFileForRead("XTC", filepath, m_file)) return failOpen(XtcError::FILE_NOT_FOUND);

  XtcError error = readHeader();
  if (error == XtcError::OK) error = readMetadata();
  if (error == XtcError::OK) error = validatePageTable();
  if (error == XtcError::OK && m_hasChapters) {
    error = readChapters();
    if (error == XtcError::OK) {
      // Validation must not retain chapter strings during normal rendering.
      m_chapters.clear();
      m_chapters.shrink_to_fit();
      m_chaptersLoaded = false;
    }
  }
  if (error != XtcError::OK) {
    LOG_DBG("XTC", "Rejected %s: %s", filepath, errorToString(error));
    return failOpen(error);
  }

  const uint32_t identityStarted = millis();
  error = fingerprintSource();
  if (error != XtcError::OK) return failOpen(error);
  LOG_DBG("XTC", "Source identity scan: %llu bytes in %lu ms", static_cast<unsigned long long>(m_fileSize),
          static_cast<unsigned long>(millis() - identityStarted));
  (void)identityStarted;

  closeFile();
  m_isOpen = true;
  m_lastError = XtcError::OK;
  LOG_DBG("XTC", "Opened file: %s (%u pages, %ux%u)", filepath, m_header.pageCount, m_defaultWidth,
          m_defaultHeight);
  return XtcError::OK;
}

void XtcParser::close() {
  closeFile();
  m_isOpen = false;
  m_fileSize = 0;
  m_chapterCount = 0;
  m_hasSourceIdentity = false;
  m_sourceIdentity = {};
  m_chaptersLoaded = false;
  m_chapters.clear();
  m_title.clear();
  m_author.clear();
  m_hasChapters = false;
  m_defaultWidth = DISPLAY_WIDTH;
  m_defaultHeight = DISPLAY_HEIGHT;
  m_bitDepth = 1;
  std::memset(&m_header, 0, sizeof(m_header));
}

bool XtcParser::ensureFileOpen() {
  if (m_file.isOpen()) return true;
  return Storage.openFileForRead("XTC", m_filepath.c_str(), m_file);
}

void XtcParser::closeFile() {
  if (m_file.isOpen()) m_file.close();
}

XtcError XtcParser::readHeader() {
  m_fileSize = m_file.fileSize64();
  if (m_fileSize < sizeof(XtcHeader) ||
      m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader)) != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }
  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) return XtcError::INVALID_MAGIC;
  if (m_header.versionMajor != 1 || m_header.versionMinor != 0) return XtcError::INVALID_VERSION;
  if (m_header.pageCount == 0 || m_header.readDirection > 2 || m_header.hasMetadata > 1 ||
      m_header.hasThumbnails > 1 || m_header.hasChapters > 1 || m_header.currentPage > m_header.pageCount) {
    return XtcError::CORRUPTED_HEADER;
  }
  if (m_header.pageTableOffset < sizeof(XtcHeader) || m_header.dataOffset < sizeof(XtcHeader) ||
      m_header.pageTableOffset > m_fileSize || m_header.dataOffset > m_fileSize) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  m_bitDepth = m_header.magic == XTCH_MAGIC ? 2 : 1;
  return XtcError::OK;
}

XtcError XtcParser::readMetadata() {
  m_title.clear();
  m_author.clear();
  m_chapterCount = 0;
  m_hasChapters = false;

  if (m_header.hasMetadata == 0) {
    return m_header.hasChapters == 0 ? XtcError::OK : XtcError::INVALID_CHAPTERS;
  }
  if (m_header.metadataOffset < sizeof(XtcHeader) ||
      !rangeWithin(m_header.metadataOffset, XTC_METADATA_SIZE, m_fileSize) ||
      !m_file.seek64(m_header.metadataOffset)) {
    return XtcError::INVALID_METADATA;
  }

  std::array<uint8_t, XTC_METADATA_SIZE> metadata{};
  const int metadataRead = m_file.read(metadata.data(), metadata.size());
  if (metadataRead < 0 || static_cast<size_t>(metadataRead) != metadata.size()) return XtcError::READ_ERROR;
  if (!hasTerminator(metadata.data(), 128) || !hasTerminator(metadata.data() + 128, 64)) {
    return XtcError::INVALID_METADATA;
  }
  m_title.assign(reinterpret_cast<const char*>(metadata.data()), strnlen(reinterpret_cast<const char*>(metadata.data()), 128));
  m_author.assign(reinterpret_cast<const char*>(metadata.data() + 128),
                  strnlen(reinterpret_cast<const char*>(metadata.data() + 128), 64));

  if (m_header.hasChapters == 0) return XtcError::OK;
  std::memcpy(&m_chapterCount, metadata.data() + 196, sizeof(m_chapterCount));
  if (m_chapterCount == 0 || m_chapterCount > m_header.pageCount || m_chapterCount > XTC_MAX_CHAPTERS) {
    return XtcError::INVALID_CHAPTERS;
  }
  uint64_t chapterBytes = static_cast<uint64_t>(m_chapterCount) * XTC_CHAPTER_SIZE;
  if (m_header.chapterOffset < sizeof(XtcHeader) ||
      !rangeWithin(m_header.chapterOffset, chapterBytes, m_fileSize)) {
    return XtcError::INVALID_CHAPTERS;
  }
  m_hasChapters = true;
  return XtcError::OK;
}

XtcError XtcParser::validatePageTable() {
  const uint64_t tableBytes = static_cast<uint64_t>(m_header.pageCount) * sizeof(PageTableEntry);
  if (!rangeWithin(m_header.pageTableOffset, tableBytes, m_fileSize)) return XtcError::OFFSET_OUT_OF_RANGE;

  for (uint32_t page = 0; page < m_header.pageCount; ++page) {
    PageInfo info;
    const XtcError error = validatePageEntry(page, &info);
    if (error != XtcError::OK) return error;
    if (page == 0) {
      m_defaultWidth = info.width;
      m_defaultHeight = info.height;
    }
  }
  return XtcError::OK;
}

XtcError XtcParser::validatePageEntry(const uint32_t pageIndex, PageInfo* info) {
  if (pageIndex >= m_header.pageCount) return XtcError::PAGE_OUT_OF_RANGE;
  if (!ensureFileOpen()) return XtcError::FILE_NOT_FOUND;

  const uint64_t entryOffset =
      m_header.pageTableOffset + static_cast<uint64_t>(pageIndex) * sizeof(PageTableEntry);
  if (!rangeWithin(entryOffset, sizeof(PageTableEntry), m_fileSize) || !m_file.seek64(entryOffset)) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }
  PageTableEntry entry{};
  if (m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) != sizeof(entry)) return XtcError::READ_ERROR;
  if (entry.width != DISPLAY_WIDTH || entry.height != DISPLAY_HEIGHT) return XtcError::UNSUPPORTED_DIMENSIONS;
  if (entry.dataOffset < m_header.dataOffset || !rangeWithin(entry.dataOffset, entry.dataSize, m_fileSize) ||
      entry.dataSize < sizeof(XtgPageHeader) || !m_file.seek64(entry.dataOffset)) {
    return XtcError::OFFSET_OUT_OF_RANGE;
  }

  XtgPageHeader pageHeader{};
  if (m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(pageHeader)) != sizeof(pageHeader)) {
    return XtcError::READ_ERROR;
  }
  const uint32_t expectedMagic = m_bitDepth == 2 ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) return XtcError::INVALID_MAGIC;
  if (pageHeader.width != entry.width || pageHeader.height != entry.height) return XtcError::SIZE_MISMATCH;
  if (pageHeader.width != DISPLAY_WIDTH || pageHeader.height != DISPLAY_HEIGHT) {
    return XtcError::UNSUPPORTED_DIMENSIONS;
  }
  if (pageHeader.colorMode != 0 || pageHeader.compression != 0) return XtcError::UNSUPPORTED_COMPRESSION;

  PageLayout layout;
  if (!calculatePageLayout(pageHeader.width, pageHeader.height, m_bitDepth, layout)) {
    return XtcError::SIZE_MISMATCH;
  }
  size_t encodedBytes = 0;
  if (!checkedAdd(sizeof(XtgPageHeader), layout.payloadBytes, encodedBytes) ||
      pageHeader.dataSize != layout.payloadBytes || entry.dataSize != encodedBytes) {
    return XtcError::SIZE_MISMATCH;
  }
  if (!rangeWithin(entry.dataOffset, encodedBytes, m_fileSize)) return XtcError::OFFSET_OUT_OF_RANGE;

  if (info) {
    info->offset = entry.dataOffset;
    info->size = entry.dataSize;
    info->width = entry.width;
    info->height = entry.height;
    info->bitDepth = m_bitDepth;
    info->padding = 0;
  }
  // Deliberately leave the cursor immediately after the page header so the
  // load methods can read the already-validated payload without a second seek.
  return XtcError::OK;
}

bool XtcParser::readPageTableEntry(const uint32_t pageIndex, PageInfo& info) {
  const XtcError error = validatePageEntry(pageIndex, &info);
  m_lastError = error;
  return error == XtcError::OK;
}

XtcError XtcParser::readChapters() {
  m_chapters.clear();
  if (!m_hasChapters) return XtcError::OK;
  if (!ensureFileOpen() || !m_file.seek64(m_header.chapterOffset)) return XtcError::READ_ERROR;

  m_chapters.reserve(m_chapterCount);
  std::array<uint8_t, XTC_CHAPTER_SIZE> bytes{};
  for (uint16_t index = 0; index < m_chapterCount; ++index) {
    const int chapterRead = m_file.read(bytes.data(), bytes.size());
    if (chapterRead < 0 || static_cast<size_t>(chapterRead) != bytes.size()) return XtcError::READ_ERROR;
    if (!hasTerminator(bytes.data(), 80)) return XtcError::INVALID_CHAPTERS;

    uint16_t startPage = 0;
    uint16_t endPage = 0;
    std::memcpy(&startPage, bytes.data() + 0x50, sizeof(startPage));
    std::memcpy(&endPage, bytes.data() + 0x52, sizeof(endPage));
    // The supported converter writes chapter page numbers as 1-based values.
    if (startPage == 0 || endPage == 0 || startPage > endPage || endPage > m_header.pageCount) {
      return XtcError::INVALID_CHAPTERS;
    }
    std::string name(reinterpret_cast<const char*>(bytes.data()),
                     strnlen(reinterpret_cast<const char*>(bytes.data()), 80));
    m_chapters.push_back({std::move(name), static_cast<uint16_t>(startPage - 1U),
                          static_cast<uint16_t>(endPage - 1U)});
  }
  return XtcError::OK;
}

const std::vector<ChapterInfo>& XtcParser::getChapters() {
  if (!m_chaptersLoaded && m_hasChapters) {
    const XtcError error = readChapters();
    if (error != XtcError::OK) {
      LOG_ERR("XTC", "Failed to load chapters: %s", errorToString(error));
      m_chapters.clear();
      m_hasChapters = false;
      m_lastError = error;
    }
    m_chaptersLoaded = true;
    closeFile();
  }
  return m_chapters;
}

bool XtcParser::getPageInfo(const uint32_t pageIndex, PageInfo& info) {
  const bool ok = readPageTableEntry(pageIndex, info);
  closeFile();
  return ok;
}

size_t XtcParser::loadPage(const uint32_t pageIndex, uint8_t* buffer, const size_t bufferSize) {
  if (!m_isOpen) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return 0;
  }
  if (!buffer) {
    m_lastError = XtcError::INVALID_ARGUMENT;
    return 0;
  }

  PageInfo page;
  m_lastError = validatePageEntry(pageIndex, &page);
  if (m_lastError != XtcError::OK) {
    closeFile();
    return 0;
  }
  PageLayout layout;
  if (!calculatePageLayout(page.width, page.height, page.bitDepth, layout) || bufferSize < layout.payloadBytes) {
    m_lastError = XtcError::MEMORY_ERROR;
    closeFile();
    return 0;
  }
  const size_t read = m_file.read(buffer, layout.payloadBytes);
  const bool sizeStable = m_file.fileSize64() == m_fileSize;
  closeFile();
  if (read != layout.payloadBytes || !sizeStable) {
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }
  m_lastError = XtcError::OK;
  return read;
}

XtcError XtcParser::loadPageStreaming(
    const uint32_t pageIndex, std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
    const size_t chunkSize) {
  if (!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if (!callback || chunkSize == 0) return XtcError::INVALID_ARGUMENT;

  PageInfo page;
  XtcError error = validatePageEntry(pageIndex, &page);
  if (error != XtcError::OK) {
    closeFile();
    return error;
  }
  PageLayout layout;
  if (!calculatePageLayout(page.width, page.height, page.bitDepth, layout)) {
    closeFile();
    return XtcError::SIZE_MISMATCH;
  }

  std::array<uint8_t, 1024> chunk{};
  const size_t boundedChunkSize = std::min(chunkSize, chunk.size());
  size_t totalRead = 0;
  while (totalRead < layout.payloadBytes) {
    const size_t wanted = std::min(boundedChunkSize, layout.payloadBytes - totalRead);
    const int read = m_file.read(chunk.data(), wanted);
    if (read <= 0 || static_cast<size_t>(read) > wanted) {
      closeFile();
      return XtcError::READ_ERROR;
    }
    callback(chunk.data(), static_cast<size_t>(read), totalRead);
    totalRead += static_cast<size_t>(read);
  }
  const bool sizeStable = m_file.fileSize64() == m_fileSize;
  closeFile();
  return sizeStable ? XtcError::OK : XtcError::READ_ERROR;
}

XtcError XtcParser::fingerprintSource() {
  if (!ensureFileOpen() || !m_file.seek64(0)) return XtcError::READ_ERROR;
  std::array<uint8_t, IDENTITY_CHUNK_SIZE> buffer{};
  uint64_t totalRead = 0;
  size_t bytesSinceYield = 0;
  uint32_t crc = UINT32_MAX;
  uint64_t fnv = FNV64_OFFSET_BASIS;
  while (totalRead < m_fileSize) {
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), m_fileSize - totalRead));
    const int read = m_file.read(buffer.data(), wanted);
    if (read <= 0 || static_cast<size_t>(read) > wanted) return XtcError::READ_ERROR;
    updateRawIdentity(buffer.data(), static_cast<size_t>(read), crc, fnv);
    totalRead += static_cast<size_t>(read);
    bytesSinceYield += static_cast<size_t>(read);
    if (bytesSinceYield >= IDENTITY_YIELD_BYTES) {
      yield();
      bytesSinceYield = 0;
    }
  }
  if (totalRead != m_fileSize || m_file.fileSize64() != m_fileSize) return XtcError::READ_ERROR;
  m_sourceIdentity = ZipFile::SourceIdentity::forRawFile(m_fileSize, ~crc, fnv);
  m_hasSourceIdentity = true;
  return XtcError::OK;
}

bool XtcParser::getSourceIdentity(ZipFile::SourceIdentity& identity) const {
  if (!m_isOpen || !m_hasSourceIdentity || !m_sourceIdentity.isRawFile()) return false;
  identity = m_sourceIdentity;
  return true;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  XtcParser parser;
  return parser.open(filepath) == XtcError::OK;
}

}  // namespace xtc
