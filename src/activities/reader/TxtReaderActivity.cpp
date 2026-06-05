#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "IsHasChapterPattern.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::scanChapters() {
  m_chapters.clear();
  m_isVolumeOnlyBook = false;
  // Chapter is 76 bytes (4 + 4 + 4 + 64). 2000 chapters = 152 KB DRAM.
  m_chapters.reserve(1000);

  constexpr size_t kChunkSize = 8192;
  constexpr size_t kMaxLineLength = 4096;
  constexpr uint32_t kMaxChapters = 2000;
  constexpr uint32_t kYieldEveryBytes = 50 * 1024;
  std::vector<uint8_t> buf(kChunkSize);
  uint32_t filePos = 0;
  std::string currentLine;
  currentLine.reserve(256);
  bool sawAnyChapter = false;
  uint32_t bytesSinceLastYield = 0;

  // Pass 1: scan for "第N章" patterns.
  while (filePos < txt->getFileSize()) {
    const size_t toRead = std::min(kChunkSize,
                                   static_cast<size_t>(txt->getFileSize() - filePos));
    if (!txt->readContent(buf.data(), filePos, toRead)) {
      LOG_ERR("TXT", "readContent failed at offset %u", filePos);
      break;
    }
    const uint32_t lineStart = filePos;
    for (size_t i = 0; i < toRead; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n' || c == '\r') {
        if (isHasChapterPattern(currentLine.c_str(),
                                static_cast<int>(currentLine.size()))) {
          if (m_chapters.size() >= kMaxChapters) break;
          Chapter ch{};
          ch.chapterIndex = static_cast<uint32_t>(m_chapters.size());
          ch.byteOffset = lineStart;
          std::strncpy(ch.shortTitle, currentLine.c_str(),
                       sizeof(ch.shortTitle) - 1);
          ch.shortTitle[sizeof(ch.shortTitle) - 1] = '\0';
          m_chapters.push_back(ch);
          sawAnyChapter = true;
        }
        currentLine.clear();
        if (c == '\r' && i + 1 < toRead && buf[i + 1] == '\n') ++i;
      } else {
        if (currentLine.size() < kMaxLineLength) {
          currentLine.push_back(c);
        }
      }
    }
    filePos += static_cast<uint32_t>(toRead);
    bytesSinceLastYield += static_cast<uint32_t>(toRead);
    if (bytesSinceLastYield >= kYieldEveryBytes) {
      vTaskDelay(1);
      bytesSinceLastYield = 0;
    }
    if (m_chapters.size() >= kMaxChapters) break;
  }

  if (sawAnyChapter && m_chapters.size() > 1) {
    for (size_t i = 0; i + 1 < m_chapters.size(); ++i) {
      m_chapters[i].endOffset = m_chapters[i + 1].byteOffset;
    }
    m_chapters.back().endOffset = static_cast<uint32_t>(txt->getFileSize());
    return;
  }

  // Volume-only fallback.
  m_chapters.clear();
  m_chapters.reserve(1000);
  m_isVolumeOnlyBook = true;
  uint32_t paraCount = 0;
  uint32_t chapterStart = 0;
  std::minstd_rand rng(static_cast<uint32_t>(txt->getFileSize()));
  uint16_t nextThreshold = VOLUME_PARAGRAPHS_BASE +
                           (rng() % VOLUME_PARAGRAPHS_JITTER);
  filePos = 0;
  currentLine.clear();
  bytesSinceLastYield = 0;

  while (filePos < txt->getFileSize()) {
    const size_t toRead = std::min(kChunkSize,
                                   static_cast<size_t>(txt->getFileSize() - filePos));
    if (!txt->readContent(buf.data(), filePos, toRead)) {
      LOG_ERR("TXT", "readContent failed at offset %u", filePos);
      break;
    }
    for (size_t i = 0; i < toRead; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        if (currentLine.empty()) {
          if (++paraCount >= nextThreshold) {
            if (m_chapters.size() >= kMaxChapters) break;
            Chapter ch{};
            ch.chapterIndex = static_cast<uint32_t>(m_chapters.size());
            ch.byteOffset = chapterStart;
            std::snprintf(ch.shortTitle, sizeof(ch.shortTitle),
                          "\xe7\xac\xac%u\xe7\xab\xa0",
                          static_cast<unsigned>(m_chapters.size() + 1));
            m_chapters.push_back(ch);
            chapterStart = filePos + 1;
            paraCount = 0;
            nextThreshold = VOLUME_PARAGRAPHS_BASE +
                            (rng() % VOLUME_PARAGRAPHS_JITTER);
          }
        }
        currentLine.clear();
      } else if (c != '\r') {
        if (currentLine.size() < kMaxLineLength) {
          currentLine.push_back(c);
        }
      }
    }
    filePos += static_cast<uint32_t>(toRead);
    bytesSinceLastYield += static_cast<uint32_t>(toRead);
    if (bytesSinceLastYield >= kYieldEveryBytes) {
      vTaskDelay(1);
      bytesSinceLastYield = 0;
    }
    if (m_chapters.size() >= kMaxChapters) break;
  }
  if (chapterStart < txt->getFileSize() && m_chapters.size() < kMaxChapters) {
    Chapter ch{};
    ch.chapterIndex = static_cast<uint32_t>(m_chapters.size());
    ch.byteOffset = chapterStart;
    ch.endOffset = static_cast<uint32_t>(txt->getFileSize());
    std::snprintf(ch.shortTitle, sizeof(ch.shortTitle),
                  "\xe7\xac\xac%u\xe7\xab\xa0",
                  static_cast<unsigned>(m_chapters.size() + 1));
    m_chapters.push_back(ch);
  }
  // Unconditional endOffset pass for volume-only chapters.
  for (size_t i = 0; i + 1 < m_chapters.size(); ++i) {
    m_chapters[i].endOffset = m_chapters[i + 1].byteOffset;
  }
  if (!m_chapters.empty()) {
    m_chapters.back().endOffset = static_cast<uint32_t>(txt->getFileSize());
  }
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;

        // Apply text alignment
        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

bool TxtReaderActivity::loadTypeCache() {
  FsFile f;
  const std::string path = txt->getCachePath() + "/type.bin";
  if (!Storage.openFileForRead("TXT", path.c_str(), f)) {
    return false;  // No cache yet — first open.
  }

  // Header: 12 bytes = magic[4] + version[1] + fileSize[4] + reserved[3].
  uint8_t header[12] = {0};
  if (f.read(header, 12) != 12) {
    LOG_DBG("TXT", "Type cache header read failed, rebuilding");
    return false;
  }
  uint32_t magic;
  std::memcpy(&magic, header, 4);
  if (magic != TYPE_CACHE_MAGIC) {
    LOG_DBG("TXT", "Type cache magic mismatch, rebuilding");
    return false;
  }
  if (header[4] != TYPE_CACHE_VERSION) {
    LOG_DBG("TXT", "Type cache version mismatch, rebuilding");
    return false;
  }

  // Validate file size — protects against stale cache after file edit.
  uint32_t fileSizeInCache;
  std::memcpy(&fileSizeInCache, header + 5, 4);
  if (fileSizeInCache != txt->getFileSize()) {
    LOG_DBG("TXT", "Type cache file size mismatch, rebuilding");
    return false;
  }

  uint8_t typeByte = 0;
  if (f.read(&typeByte, 1) != 1) {
    LOG_DBG("TXT", "Type cache data read failed, rebuilding");
    return false;
  }
  // typeByte is 0 (regular) or 1 (volume-only). Any other value is corrupt.
  if (typeByte > 1) {
    LOG_DBG("TXT", "Type cache has unknown type byte %u, rebuilding", typeByte);
    return false;
  }
  m_isVolumeOnlyBook = (typeByte == 1);
  return true;
}

void TxtReaderActivity::saveTypeCache() const {
  FsFile f;
  const std::string path = txt->getCachePath() + "/type.bin";
  if (!Storage.openFileForWrite("TXT", path.c_str(), f)) {
    LOG_DBG("TXT", "Failed to open type cache for write");
    return;
  }

  // Header: 12 bytes = magic[4] + version[1] + fileSize[4] + reserved[3].
  uint8_t header[12] = {0};
  uint32_t magic = TYPE_CACHE_MAGIC;
  std::memcpy(header, &magic, 4);
  header[4] = TYPE_CACHE_VERSION;
  const uint32_t fileSize = static_cast<uint32_t>(txt->getFileSize());
  std::memcpy(header + 5, &fileSize, 4);
  f.write(header, 12);
  const uint8_t typeByte = m_isVolumeOnlyBook ? 1 : 0;
  f.write(&typeByte, 1);
}

bool TxtReaderActivity::loadChapterCache() {
  FsFile f;
  const std::string path = txt->getCachePath() + "/chapters.bin";
  if (!Storage.openFileForRead("TXT", path.c_str(), f)) {
    return false;  // No cache yet — first open.
  }

  // Header: 12 bytes = magic[4] + version[1] + fileSize[4] + reserved[3].
  uint8_t header[12] = {0};
  if (f.read(header, 12) != 12) {
    LOG_DBG("TXT", "Chapter cache header read failed, rebuilding");
    return false;
  }
  uint32_t magic;
  std::memcpy(&magic, header, 4);
  if (magic != CHAPTER_CACHE_MAGIC) {
    LOG_DBG("TXT", "Chapter cache magic mismatch, rebuilding");
    return false;
  }
  if (header[4] != CHAPTER_CACHE_VERSION) {
    LOG_DBG("TXT", "Chapter cache version mismatch, rebuilding");
    return false;
  }
  // Validate file size — critical for offset-based caches.
  uint32_t fileSizeInCache;
  std::memcpy(&fileSizeInCache, header + 5, 4);
  if (fileSizeInCache != txt->getFileSize()) {
    LOG_DBG("TXT", "Chapter cache file size mismatch, rebuilding");
    return false;
  }

  uint32_t count = 0;
  if (f.read(&count, 4) != 4) {
    LOG_DBG("TXT", "Chapter cache count read failed, rebuilding");
    return false;
  }
  if (count > 2000) {
    LOG_DBG("TXT", "Chapter cache count %u out of range, rebuilding", count);
    return false;
  }
  m_chapters.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    Chapter& ch = m_chapters[i];
    if (f.read(&ch.chapterIndex, 4) != 4) {
      LOG_DBG("TXT", "Chapter %u: chapterIndex read failed, rebuilding", i);
      return false;
    }
    if (f.read(&ch.byteOffset, 4) != 4) {
      LOG_DBG("TXT", "Chapter %u: byteOffset read failed, rebuilding", i);
      return false;
    }
    if (f.read(&ch.endOffset, 4) != 4) {
      LOG_DBG("TXT", "Chapter %u: endOffset read failed, rebuilding", i);
      return false;
    }
    if (f.read(ch.shortTitle, sizeof(ch.shortTitle)) != sizeof(ch.shortTitle)) {
      LOG_DBG("TXT", "Chapter %u: shortTitle read failed, rebuilding", i);
      return false;
    }
    // Validate field invariants.
    if (ch.chapterIndex != i) {
      LOG_DBG("TXT", "Chapter %u: chapterIndex invariant violated, rebuilding", i);
      return false;
    }
    if (ch.byteOffset > ch.endOffset) {
      LOG_DBG("TXT", "Chapter %u: byteOffset > endOffset, rebuilding", i);
      return false;
    }
    if (ch.endOffset > txt->getFileSize()) {
      LOG_DBG("TXT", "Chapter %u: endOffset out of bounds, rebuilding", i);
      return false;
    }
  }
  return true;
}

void TxtReaderActivity::saveChapterCache() {
  FsFile f;
  const std::string path = txt->getCachePath() + "/chapters.bin";
  if (!Storage.openFileForWrite("TXT", path.c_str(), f)) {
    LOG_DBG("TXT", "Failed to open chapter cache for write");
    return;
  }

  // Header: 12 bytes = magic[4] + version[1] + fileSize[4] + reserved[3].
  uint8_t header[12] = {0};
  uint32_t magic = CHAPTER_CACHE_MAGIC;
  std::memcpy(header, &magic, 4);
  header[4] = CHAPTER_CACHE_VERSION;
  const uint32_t fileSize = static_cast<uint32_t>(txt->getFileSize());
  std::memcpy(header + 5, &fileSize, 4);
  f.write(header, 12);

  const uint32_t count = static_cast<uint32_t>(m_chapters.size());
  f.write(&count, 4);
  for (const auto& ch : m_chapters) {
    f.write(&ch.chapterIndex, 4);
    f.write(&ch.byteOffset, 4);
    f.write(&ch.endOffset, 4);
    f.write(ch.shortTitle, sizeof(ch.shortTitle));
  }
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
