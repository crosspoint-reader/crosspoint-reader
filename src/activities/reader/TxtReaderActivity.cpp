#include "TxtReaderActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 2;          // Increment when cache format changes
constexpr uint32_t MAX_CACHE_PAGES = 65535;   // Sanity cap to prevent unbounded reserve()

// Parses and word-wraps lines from a file chunk into outLines.
// Returns the number of bytes consumed from the start of buffer.
size_t parseAndWrapLines(const uint8_t* buffer, size_t chunkSize, size_t fileOffset, size_t fileSize, int linesPerPage,
                         GfxRenderer& renderer, int fontId, int vw, std::vector<std::string>& outLines) {
  size_t pos = 0;
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;
    bool lineComplete = (lineEnd < chunkSize) || (fileOffset + lineEnd >= fileSize);
    if (!lineComplete && !outLines.empty()) break;

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;
    std::string line(reinterpret_cast<const char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      if (renderer.getTextWidth(fontId, line.c_str()) <= vw) {
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextWidth(fontId, line.substr(0, breakPos).c_str()) > vw) {
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          breakPos--;
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) breakPos--;
        }
      }
      if (breakPos == 0) {
        breakPos = 1;
        while (breakPos < line.length() && (line[breakPos] & 0xC0) == 0x80) breakPos++;
      }
      outLines.push_back(line.substr(0, breakPos));
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') skipChars++;
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

    if (line.empty()) {
      pos = lineEnd + 1;
    } else {
      pos = pos + lineBytePos;
      break;
    }
  }
  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }
  return pos;
}
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

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
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

  size_t pos = parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, cachedFontId,
                                 viewportWidth, outLines);
  nextOffset = offset + pos;
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
  renderer.clearFontCache();

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
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
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

  // First pass: BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
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
    // 6-byte format: page(2 bytes LE) + file offset(4 bytes LE)
    // The offset lets drawCurrentPageToBuffer render without requiring index.bin.
    const size_t offset = (currentPage < static_cast<int>(pageOffsets.size())) ? pageOffsets[currentPage] : 0;
    uint8_t data[6];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = offset & 0xFF;
    data[3] = (offset >> 8) & 0xFF;
    data[4] = (offset >> 16) & 0xFF;
    data[5] = (offset >> 24) & 0xFF;
    f.write(data, 6);
    f.close();
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
    f.close();
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
    f.close();
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    f.close();
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    f.close();
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    f.close();
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    f.close();
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);
  if (numPages > MAX_CACHE_PAGES) {
    LOG_ERR("TRS", "Cache numPages %u exceeds cap %u, cache invalid", numPages, MAX_CACHE_PAGES);
    f.close();
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  f.close();
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

  f.close();
  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

bool TxtReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Txt txt(filePath, "/.crosspoint");
  if (!txt.load()) {
    LOG_DBG("SLP", "TXT: failed to load %s", filePath.c_str());
    return false;
  }

  // Apply the reader orientation so margins match what the reader would produce
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  // Compute layout values that match what initializeReader() produces
  const int fontId = SETTINGS.getReaderFontId();
  const uint8_t screenMargin = SETTINGS.screenMargin;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += screenMargin;
  marginLeft += screenMargin;
  marginRight += screenMargin;
  marginBottom += std::max(screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const int vw = renderer.getScreenWidth() - marginLeft - marginRight;
  const int vh = renderer.getScreenHeight() - marginTop - marginBottom;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int linesPerPage = std::max(1, vh / lineHeight);

  // Step 1: Try to read the saved page and its file offset from progress.bin.
  // The 6-byte format (written by saveProgress) stores: page(2) + offset(4).
  // This lets us skip index.bin entirely, so the overlay works even when the
  // page index cache is missing or stale (e.g. after a firmware update).
  int savedPage = 0;
  size_t savedOffset = 0;
  bool offsetKnown = false;
  {
    FsFile progFile;
    if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", progFile)) {
      uint8_t data[6] = {0};
      const int n = progFile.read(data, 6);
      progFile.close();
      if (n >= 2) {
        savedPage = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      }
      if (n >= 6) {
        const uint32_t off =
            (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        if (off < txt.getFileSize()) {
          savedOffset = off;
          offsetKnown = true;
        }
      }
    }
  }

  // Step 2: If progress.bin didn't provide the offset, fall back to index.bin.
  if (!offsetKnown) {
    std::string cachePath = txt.getCachePath() + "/index.bin";
    FsFile cacheFile;
    if (Storage.openFileForRead("SLP", cachePath, cacheFile)) {
      uint32_t magic;
      serialization::readPod(cacheFile, magic);
      uint8_t version;
      serialization::readPod(cacheFile, version);
      uint32_t cachedFileSize;
      serialization::readPod(cacheFile, cachedFileSize);
      int32_t cachedVw, cachedLpp, cachedFontId, cachedMargin;
      serialization::readPod(cacheFile, cachedVw);
      serialization::readPod(cacheFile, cachedLpp);
      serialization::readPod(cacheFile, cachedFontId);
      serialization::readPod(cacheFile, cachedMargin);
      uint8_t cachedAlignment;
      serialization::readPod(cacheFile, cachedAlignment);
      uint32_t numPages;
      serialization::readPod(cacheFile, numPages);

      if (magic == CACHE_MAGIC && version == CACHE_VERSION && cachedFileSize == txt.getFileSize() && cachedVw == vw &&
          cachedLpp == linesPerPage && cachedFontId == fontId && cachedMargin == screenMargin &&
          cachedAlignment == paragraphAlignment && numPages > 0 && numPages <= MAX_CACHE_PAGES) {
        if (savedPage < 0 || savedPage >= static_cast<int>(numPages)) savedPage = 0;
        for (uint32_t i = 0; i < numPages; i++) {
          uint32_t off;
          serialization::readPod(cacheFile, off);
          if (static_cast<int>(i) == savedPage) {
            if (off < txt.getFileSize()) {
              savedOffset = off;
              offsetKnown = true;
            } else {
              LOG_DBG("SLP", "TXT: index.bin offset %u out of range (fileSize=%u), ignoring", off, txt.getFileSize());
            }
          }
        }
      } else {
        LOG_DBG("SLP", "TXT: index cache invalid or stale");
      }
      cacheFile.close();
    }

    // Step 3: No valid cache at all — render from the start of the file as a last resort.
    // This shows page 1 rather than a blank screen, which is always preferable.
    if (!offsetKnown) {
      LOG_DBG("SLP", "TXT: no valid cache, falling back to start of file");
      savedOffset = 0;
    }
  }

  // Load the page lines from file
  std::vector<std::string> pageLines;
  const size_t fileSize = txt.getFileSize();
  size_t offset = savedOffset;
  if (offset >= fileSize) {
    LOG_DBG("SLP", "TXT: page offset out of bounds");
    return false;
  }

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) return false;

  if (!txt.readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, fontId, vw, pageLines);
  free(buffer);

  if (pageLines.empty()) return false;

  // Render lines to frame buffer (no displayBuffer call)
  renderer.clearScreen();
  int y = marginTop;
  for (const auto& line : pageLines) {
    if (!line.empty()) {
      int x = marginLeft;
      switch (paragraphAlignment) {
        case CrossPointSettings::CENTER_ALIGN:
          x = marginLeft + (vw - renderer.getTextWidth(fontId, line.c_str())) / 2;
          break;
        case CrossPointSettings::RIGHT_ALIGN:
          x = marginLeft + vw - renderer.getTextWidth(fontId, line.c_str());
          break;
        default:
          break;
      }
      renderer.drawText(fontId, x, y, line.c_str());
    }
    y += lineHeight;
  }
  return true;
}
