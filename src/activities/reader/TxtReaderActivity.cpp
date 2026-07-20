#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <BufferedFile.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ProgressFileCodec.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "TxtLineWrap.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// The legacy scan is cheaper when a short line only barely overflows.
constexpr size_t MIN_BINARY_WRAP_BYTES = 96;
constexpr size_t CACHE_IO_BUFFER_SIZE = 1024;
constexpr size_t CACHE_HEADER_SIZE = 30;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when pagination behavior changes
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
  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
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
    if (buildPageIndex() && !pageOffsets.empty()) {
      // Save only complete indexes. A transient read/allocation failure must
      // not persist a permanently truncated view of the book.
      savePageIndexCache();
    }
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

bool TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    totalPages = 0;
    return true;
  }

  pageOffsets.push_back(0);  // First page starts at offset 0

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  std::vector<std::string> tempLines;
  tempLines.reserve(linesPerPage);
  while (offset < fileSize) {
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
  return offset >= fileSize;
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
    bool useMonotonicPrefixSearch = line.size() >= MIN_BINARY_WRAP_BYTES &&
                                    renderer.hasSdCardAdvanceTable(cachedFontId) &&
                                    TxtLineWrap::isMonotonicLtrText(line);

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

      // SD-card font advances are unsigned and additive. For simple LTR
      // ASCII/CJK text, find the same largest fitting prefix in O(log n)
      // measurements instead of walking backward through every codepoint.
      size_t breakPos = 0;
      if (useMonotonicPrefixSearch) {
        breakPos = TxtLineWrap::findLargestFittingPrefix(line, viewportWidth, [&](const char* prefix) {
          return renderer.getTextAdvanceX(cachedFontId, prefix, EpdFontFamily::REGULAR);
        });
        if (breakPos == 0) {
          useMonotonicPrefixSearch = false;
        } else {
          breakPos = TxtLineWrap::preserveWordBreak(line, breakPos);
        }
      }

      // Preserve the existing word-break path for all other text and for the
      // defensive case where even the first codepoint does not fit.
      if (breakPos == 0) {
        breakPos = line.length();
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
      }

      if (breakPos == 0) {
        breakPos = TxtLineWrap::nextUtf8Boundary(line, 0);
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line.erase(0, skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line. Move past a newline only when this
      // chunk actually contains one; otherwise the next chunk starts exactly
      // at lineEnd.
      pos = lineEnd + (lineEnd < chunkSize ? 1 : 0);
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

  if (currentPage != lastSavedPage && saveProgress()) {
    lastSavedPage = currentPage;
  }
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
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
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

bool TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  ProgressFileCodec::encodePage(static_cast<uint32_t>(currentPage), data);
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
    return false;
  }
  return true;
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      const uint32_t savedPage = ProgressFileCodec::decodePage(data);
      if (totalPages > 0 && savedPage < static_cast<uint32_t>(totalPages)) {
        currentPage = static_cast<int>(savedPage);
        lastSavedPage = currentPage;
      } else {
        currentPage = std::max(totalPages - 1, 0);
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
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  const size_t cacheSize = f.size();
  if (cacheSize < CACHE_HEADER_SIZE) {
    LOG_DBG("TRS", "Page index cache is truncated, rebuilding");
    return false;
  }

  serialization::BufferedFileReader in(f, CACHE_IO_BUFFER_SIZE);

  // Read and validate header using serialization module
  uint32_t magic = 0;
  serialization::readPod(in, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(in, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize = 0;
  serialization::readPod(in, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth = 0;
  serialization::readPod(in, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines = 0;
  serialization::readPod(in, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId = 0;
  serialization::readPod(in, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin = 0;
  serialization::readPod(in, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment = 0;
  serialization::readPod(in, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages = 0;
  serialization::readPod(in, numPages);
  const uint64_t expectedCacheSize = CACHE_HEADER_SIZE + static_cast<uint64_t>(numPages) * sizeof(uint32_t);
  const uint64_t maxPossiblePages = static_cast<uint64_t>(txt->getFileSize()) + 1;
  if (expectedCacheSize != cacheSize || numPages == 0 || numPages > maxPossiblePages) {
    LOG_DBG("TRS", "Page index cache has invalid size or page count, rebuilding");
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  uint32_t previousOffset = 0;
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t pageOffset = 0;
    serialization::readPod(in, pageOffset);
    const bool invalidFirstOffset = i == 0 && pageOffset != 0;
    const bool invalidLaterOffset = i > 0 && pageOffset <= previousOffset;
    const bool pastContent = fileSize == 0 ? pageOffset != 0 : pageOffset >= fileSize;
    if (invalidFirstOffset || invalidLaterOffset || pastContent) {
      pageOffsets.clear();
      LOG_DBG("TRS", "Page index cache has invalid offsets, rebuilding");
      return false;
    }
    pageOffsets.push_back(pageOffset);
    previousOffset = pageOffset;
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  serialization::BufferedFileWriter out(f, CACHE_IO_BUFFER_SIZE);

  // Write header using serialization module
  serialization::writePod(out, CACHE_MAGIC);
  serialization::writePod(out, CACHE_VERSION);
  serialization::writePod(out, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(out, static_cast<int32_t>(viewportWidth));
  serialization::writePod(out, static_cast<int32_t>(linesPerPage));
  serialization::writePod(out, static_cast<int32_t>(cachedFontId));
  serialization::writePod(out, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(out, cachedParagraphAlignment);
  serialization::writePod(out, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(out, static_cast<uint32_t>(offset));
  }

  if (!out.flush()) {
    LOG_ERR("TRS", "Failed to write page index cache");
    return;
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
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
