/**
 * XtcParser.cpp
 *
 * XTC file parsing implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "XtcParser.h"

#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <cstring>

namespace xtc {

XtcParser::XtcParser()
    : m_isOpen(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_lastError(XtcError::OK),
      m_loadBatchSize(500),  // ✅ 修改：批次大小改为2000（你的要求）
      m_loadedMaxPage(0),
      m_loadedStartPage(0) {  // ✅ 新增：只加这1个变量，记录当前页表的起始页
  memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { close(); }

XtcError XtcParser::open(const char* filepath) {
  // Close if already open
  if (m_isOpen) {
    close();
  }

  // Open file
  if (!SdMan.openFileForRead("XTC", filepath, m_file)) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Read header
  m_lastError = readHeader();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read header: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // Read title if available
  readTitle();

  // Read page table (默认只加载第一批：前10页)
  m_lastError = readPageTable();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read page table: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // Read chapters if present (单章节逻辑不变)
  m_lastError = readChapters();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read chapters: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  m_isOpen = true;
  Serial.printf("[%lu] [XTC] Opened file: %s (total pages=%u, loaded pages=[0~%u], %dx%d)\n", millis(), filepath, 
                m_header.pageCount, m_loadedMaxPage, m_defaultWidth, m_defaultHeight);
  return XtcError::OK;
}

void XtcParser::close() {
  if (m_isOpen) {
    m_file.close();
    m_isOpen = false;
  }
  m_pageTable.clear();
  m_chapters.clear();
  m_title.clear();
  m_hasChapters = false;
  m_loadedMaxPage = 0; 
  memset(&m_header, 0, sizeof(m_header));
}

XtcError XtcParser::readHeader() {
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader));
  if (bytesRead != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }

  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) {
    Serial.printf("[%lu] [XTC] Invalid magic: 0x%08X (expected 0x%08X or 0x%08X)\n", millis(), m_header.magic,
                  XTC_MAGIC, XTCH_MAGIC);
    return XtcError::INVALID_MAGIC;
  }

  m_bitDepth = (m_header.magic == XTCH_MAGIC) ? 2 : 1;

  const bool validVersion = m_header.versionMajor == 1 && m_header.versionMinor == 0 ||
                            m_header.versionMajor == 0 && m_header.versionMinor == 1;
  if (!validVersion) {
    Serial.printf("[%lu] [XTC] Unsupported version: %u.%u\n", millis(), m_header.versionMajor, m_header.versionMinor);
    return XtcError::INVALID_VERSION;
  }

  if (m_header.pageCount == 0) {
    return XtcError::CORRUPTED_HEADER;
  }

  Serial.printf("[%lu] [XTC] Header: magic=0x%08X (%s), ver=%u.%u, total pages=%u, bitDepth=%u\n", millis(), m_header.magic,
                (m_header.magic == XTCH_MAGIC) ? "XTCH" : "XTC", m_header.versionMajor, m_header.versionMinor,
                m_header.pageCount, m_bitDepth);

  return XtcError::OK;
}

XtcError XtcParser::readTitle() {
  if (m_header.titleOffset == 0) {
    m_header.titleOffset = 0x38;
  }

  if (!m_file.seek(m_header.titleOffset)) {
    return XtcError::READ_ERROR;
  }

  char titleBuf[128] = {0};
  m_file.read(reinterpret_cast<uint8_t*>(&titleBuf), sizeof(titleBuf) - 1);
  m_title = titleBuf;

  Serial.printf("[%lu] [XTC] Title: %s\n", millis(), m_title.c_str());
  return XtcError::OK;
}

//加载下一部分
XtcError XtcParser::readPageTable() {
  m_pageTable.clear();          
  m_pageTable.shrink_to_fit();  
  if (m_header.pageTableOffset == 0) {
    Serial.printf("[%lu] [XTC] Page table offset is 0, cannot read\n", millis());
    return XtcError::CORRUPTED_HEADER;
  }

  if (!m_file.seek(m_header.pageTableOffset)) {
    Serial.printf("[%lu] [XTC] Failed to seek to page table at %llu\n", millis(), m_header.pageTableOffset);
    return XtcError::READ_ERROR;
  }

  // 初始加载：从第0页开始，加载第一批10页
  uint16_t startPage = 0;
  uint16_t endPage = startPage + m_loadBatchSize - 1;
  if(endPage >= m_header.pageCount) endPage = m_header.pageCount - 1;
  uint16_t loadCount = endPage - startPage + 1;

  m_pageTable.resize(endPage + 1); // 扩容vector，保留已加载数据

  for (uint16_t i = startPage; i <= endPage; i++) {
    PageTableEntry entry;
    size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
    if (bytesRead != sizeof(PageTableEntry)) {
      Serial.printf("[%lu] [XTC] Failed to read page table entry %u\n", millis(), i);
      return XtcError::READ_ERROR;
    }

    m_pageTable[i].offset = static_cast<uint32_t>(entry.dataOffset);
    m_pageTable[i].size = entry.dataSize;
    m_pageTable[i].width = entry.width;
    m_pageTable[i].height = entry.height;
    m_pageTable[i].bitDepth = m_bitDepth;

    if (i == 0) {
      m_defaultWidth = entry.width;
      m_defaultHeight = entry.height;
    }
  }

  m_loadedMaxPage = endPage; // 更新已加载的最大页码
  Serial.printf("[%lu] [XTC] 初始化加载页表: 成功加载 [0~%u] 共%u页\n", millis(), m_loadedMaxPage, loadCount);
  return XtcError::OK;
}

// 原函数不变，保证不崩溃
XtcError XtcParser::readChapters() {
  m_hasChapters = false;
  m_chapters.clear();

  uint8_t hasChaptersFlag = 0;
  if (!m_file.seek(0x0B)) {
    return XtcError::READ_ERROR;
  }
  if (m_file.read(&hasChaptersFlag, sizeof(hasChaptersFlag)) != sizeof(hasChaptersFlag)) {
    return XtcError::READ_ERROR;
  }

  if (hasChaptersFlag != 1) {}
  uint64_t chapterOffset = 0;
  if (!m_file.seek(0x30)) {return XtcError::READ_ERROR;}
  if (m_file.read(reinterpret_cast<uint8_t*>(&chapterOffset), sizeof(chapterOffset)) != sizeof(chapterOffset)) {return XtcError::READ_ERROR;}
  if (chapterOffset == 0) {}

  const uint64_t fileSize = m_file.size();
  if (chapterOffset < sizeof(XtcHeader) || chapterOffset >= fileSize || chapterOffset + 96 > fileSize) {}

  uint64_t maxOffset = 0;
  if (m_header.pageTableOffset > chapterOffset) {maxOffset = m_header.pageTableOffset;}
  else if (m_header.dataOffset > chapterOffset) {maxOffset = m_header.dataOffset;}
  else {maxOffset = fileSize;}
  if (maxOffset <= chapterOffset) {}

  constexpr size_t chapterSize = 96;
  const uint64_t available = maxOffset - chapterOffset;
  const size_t chapterCount = static_cast<size_t>(available / chapterSize);
  if (chapterCount == 0) {}

  if (!m_file.seek(chapterOffset)) {return XtcError::READ_ERROR;}
  std::vector<uint8_t> chapterBuf(chapterSize);
  for (size_t i = 0; i < chapterCount; i++) {
    if (m_file.read(chapterBuf.data(), chapterSize) != chapterSize) {return XtcError::READ_ERROR;}
  }

  // 单章节：名称=书名/全书，页码=0~总页数-1 (逻辑上包含全书，不影响阅读)
  std::string chapterName = m_title.empty() ? "全书" : m_title;
  ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
  m_chapters.push_back(std::move(singleChapter));
  m_hasChapters = !m_chapters.empty();

  Serial.printf("[%lu] [XTC] 解析章节 #01 : 名称=[%s] | 包含全书共%u页\n", millis(), singleChapter.name.c_str(), m_header.pageCount);
  Serial.printf("[%lu] [XTC] 解析完成 ✔️  共加载有效章节数: %u\n", millis(), static_cast<unsigned int>(m_chapters.size()));
  return XtcError::OK;
}

// 主要更改部分
XtcError XtcParser::loadNextPageBatch() {
  if(!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if(m_loadedMaxPage >= m_header.pageCount - 1) {
    Serial.printf("[XTC] 已加载全部%u页\n", m_header.pageCount);
    return XtcError::PAGE_OUT_OF_RANGE;
  }

  return loadPageBatchByStart(m_loadedMaxPage + 1);
}


uint16_t XtcParser::getLoadedMaxPage() const {
  return m_loadedMaxPage;
}


uint16_t XtcParser::getPageBatchSize() const {
  return m_loadBatchSize;
}


bool XtcParser::getPageInfo(uint32_t pageIndex, PageInfo& info) const {
  if (pageIndex >= m_header.pageCount) return false;
  uint16_t targetStart = (pageIndex / m_loadBatchSize) * m_loadBatchSize;
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    auto* self = const_cast<XtcParser*>(this);
    self->loadPageBatchByStart(targetStart);
  }
  uint16_t idx = pageIndex - m_loadedStartPage;
  if(idx >= m_pageTable.size()) return false;
  info = m_pageTable[idx];
  return true;
}

//主要更改：利用现有规律提取需要的xtc页面
size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) {
  if (!m_isOpen || pageIndex >= m_header.pageCount) { 
    m_lastError = (pageIndex >= m_header.pageCount) ? XtcError::PAGE_OUT_OF_RANGE : XtcError::FILE_NOT_FOUND;
    return 0;
  }

  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    loadNextPageBatch();
  }

  uint16_t idx = pageIndex - m_loadedStartPage;
  const PageInfo& page = m_pageTable[idx]; // 替换原 m_pageTable[pageIndex]
  if (!m_file.seek(page.offset)) {
    Serial.printf("[%lu] [XTC] Failed to seek to page %u at offset %lu\n", millis(), pageIndex, page.offset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  if (headerRead != sizeof(XtgPageHeader)) {
    Serial.printf("[%lu] [XTC] Failed to read page header for page %u\n", millis(), pageIndex);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (pageHeader.magic != expectedMagic) {
    Serial.printf("[%lu] [XTC] Invalid page magic for page %u: 0x%08X (expected 0x%08X)\n", millis(), pageIndex,
                  pageHeader.magic, expectedMagic);
    m_lastError = XtcError::INVALID_MAGIC;
    return 0;
  }

  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  if (bufferSize < bitmapSize) {
    Serial.printf("[%lu] [XTC] Buffer too small: need %u, have %u\n", millis(), bitmapSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
  }

  size_t bytesRead = m_file.read(buffer, bitmapSize);
  if (bytesRead != bitmapSize) {
    Serial.printf("[%lu] [XTC] Page read error: expected %u, got %u\n", millis(), bitmapSize, bytesRead);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }

  m_lastError = XtcError::OK;
  return bytesRead;
}

XtcError XtcParser::loadPageStreaming(uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      size_t chunkSize) {
  if (!m_isOpen || pageIndex > m_loadedMaxPage || pageIndex >= m_header.pageCount) {
    return (pageIndex >= m_header.pageCount) ? XtcError::PAGE_OUT_OF_RANGE : XtcError::FILE_NOT_FOUND;
  }

  const PageInfo& page = m_pageTable[pageIndex];
  if (!m_file.seek(page.offset)) {return XtcError::READ_ERROR;}

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (headerRead != sizeof(XtgPageHeader) || pageHeader.magic != expectedMagic) {return XtcError::READ_ERROR;}

  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  std::vector<uint8_t> chunk(chunkSize);
  size_t totalRead = 0;
  while (totalRead < bitmapSize) {
    size_t toRead = std::min(chunkSize, bitmapSize - totalRead);
    size_t bytesRead = m_file.read(chunk.data(), toRead);
    if (bytesRead == 0) return XtcError::READ_ERROR;
    callback(chunk.data(), bytesRead, totalRead);
    totalRead += bytesRead;
  }
  return XtcError::OK;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  FsFile file;
  if (!SdMan.openFileForRead("XTC", filepath, file)) return false;
  uint32_t magic = 0;
  size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  file.close();
  return (bytesRead == sizeof(magic)) && (magic == XTC_MAGIC || magic == XTCH_MAGIC);
}
//换用新函数来提取章节
XtcError XtcParser::readChapters_gd(uint16_t chapterStart) {
    chapterActualCount = 0;
    memset(ChapterList, 0, sizeof(ChapterList));
    Serial.printf("[Memory] ✅ 解析前：所有章节数据内存已彻底释放\n");

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
      Serial.printf("[%lu] [XTC] 位置1");

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
      Serial.printf("[%lu] [XTC] 位置2");

  const uint64_t fileSize = m_file.size();
  if (chapterOffset < sizeof(XtcHeader) || chapterOffset >= fileSize || chapterOffset + 96 > fileSize) {
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
  constexpr size_t chapterSize = 96;
  const uint64_t available = maxOffset - chapterOffset;
  const size_t chapterCount = static_cast<size_t>(available / chapterSize);
  if (chapterCount == 0) {
    return XtcError::OK;
  }
    Serial.printf("[%lu] [XTC] 位置3");
  // 计算起始章节的偏移：章节区开头 + 起始章节索引 * 单章96字节
  uint64_t startReadOffset = chapterOffset + (chapterStart * chapterSize);
  if (!m_file.seek(startReadOffset)) { // 跳到要读取的起始章节位置
    return XtcError::READ_ERROR;
  }
    Serial.printf("[%lu] [XTC] 位置4");

  std::vector<uint8_t> chapterBuf(chapterSize);
  int readCount = 0; // 已读取的章节数，最多读25章
  size_t currentChapterIdx = chapterStart; // 当前读到的章节索引

  // 循环条件：最多读25章 + 不超过总章节数
  Serial.printf("[%lu] [XTC] readCount:%d,currentChapterIdx:%d, chapterCount %u\n", millis(), readCount, currentChapterIdx,chapterCount);
  while (readCount < 25 && currentChapterIdx < chapterCount) {
    if (m_file.read(chapterBuf.data(), chapterSize) != chapterSize) {
      break; // 读失败则退出，不返回错误，保证能读到已读的有效章节
    }

    // 解析章节名：原版逻辑
    char nameBuf[81];
    memcpy(nameBuf, chapterBuf.data(), 80);
    nameBuf[80] = '\0';
    const size_t nameLen = strnlen(nameBuf, 80);
    std::string name(nameBuf, nameLen);

    // 解析页码：原版逻辑
    uint16_t startPage = 0;
    uint16_t endPage = 0;
    memcpy(&startPage, chapterBuf.data() + 0x50, sizeof(startPage));
    memcpy(&endPage, chapterBuf.data() + 0x52, sizeof(endPage));

    // 无效章节过滤：原版逻辑
    if (name.empty() && startPage == 0 && endPage == 0) {
      currentChapterIdx++;
      continue;
    }
    if (startPage > 0) {
      startPage--;
    }
    if (endPage > 0) {
      endPage--;
    }
    if (startPage >= m_header.pageCount || startPage > endPage) {
      currentChapterIdx++;
      continue;
    }
    if (endPage >= m_header.pageCount) {
      endPage = m_header.pageCount - 1;
    }

    // 存入数组：当前读取的章节 → 数组的第readCount位
  strncpy(ChapterList[readCount].shortTitle, name.c_str(), 63);
  ChapterList[readCount].shortTitle[63] = '\0';
  ChapterList[readCount].startPage = startPage;
  ChapterList[readCount].chapterIndex = currentChapterIdx;
    
    Serial.printf("[%lu] [XTC] 第%d章，名字为:%s %u\n", millis(), readCount, ChapterList[readCount].shortTitle);
    readCount++;        // 数组索引+1
    currentChapterIdx++; // 章节索引+1

  }

  m_hasChapters = readCount > 0;
  Serial.printf("[%lu] [XTC] 翻页读取章节：起始=%d，有效数=%u\n", millis(), chapterStart, (unsigned int)readCount);
  return XtcError::OK;
}
XtcError XtcParser::loadPageBatchByStart(uint16_t startPage) {
  if(!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if(startPage >= m_header.pageCount) return XtcError::PAGE_OUT_OF_RANGE;


  m_pageTable.clear();
  m_pageTable.shrink_to_fit();


  m_loadedStartPage = startPage;
  uint16_t endPage = startPage + m_loadBatchSize - 1;
  if(endPage >= m_header.pageCount) endPage = m_header.pageCount - 1;
  uint16_t loadCount = endPage - startPage + 1;

  // 定位到指定批次的页表位置
  uint64_t seekOffset = m_header.pageTableOffset + (startPage * sizeof(PageTableEntry));
  if(!m_file.seek(seekOffset)) return XtcError::READ_ERROR;

  // 加载新批次数据（只存2000页，内存恒定）
  m_pageTable.resize(loadCount);
  for(uint16_t i = startPage; i <= endPage; i++) {
    PageTableEntry entry;
    if(m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry)) != sizeof(PageTableEntry)) {
      return XtcError::READ_ERROR;
    }
    m_pageTable[i - startPage].offset = static_cast<uint32_t>(entry.dataOffset);
    m_pageTable[i - startPage].size = entry.dataSize;
    m_pageTable[i - startPage].width = entry.width;
    m_pageTable[i - startPage].height = entry.height;
    m_pageTable[i - startPage].bitDepth = m_bitDepth;
  }

  m_loadedMaxPage = endPage; 
  Serial.printf("[XTC] 强制加载批次 : 清空旧表 → 加载 [%u~%u] | 内存占用恒定\n", startPage, endPage);
  return XtcError::OK;
}
}  // namespace xtc
