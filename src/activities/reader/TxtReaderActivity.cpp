#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
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
// Progress file magic and version. No longer stores a page-offset index — only
// the current byte offset plus the layout settings used to produce it, so we
// can reset to the start if the user changed font/margin/etc. since last read.
constexpr uint32_t PROGRESS_MAGIC = 0x54585450;  // "TXTP"
constexpr uint8_t PROGRESS_VERSION = 1;

// Find UTF-8 character boundary at or before pos
size_t findUtf8Boundary(const std::string& str, size_t pos) {
  if (pos >= str.length()) return str.length();
  // Move back if we're in the middle of a UTF-8 sequence
  while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
    pos--;
  }
  return pos;
}

// Binary search to find max characters that fit in width
// Returns the position (byte offset) where to break the string
size_t findBreakPosition(const GfxRenderer& renderer, int fontId, const std::string& line, int maxWidth) {
  if (line.empty()) return 0;

  // First check if the whole line fits
  int fullWidth = renderer.getTextWidth(fontId, line.c_str());
  if (fullWidth <= maxWidth) {
    return line.length();
  }

  // Binary search for the break point
  size_t low = 1;  // At minimum 1 character
  size_t high = line.length();
  size_t bestFit = 1;

  while (low < high) {
    size_t mid = (low + high + 1) / 2;
    mid = findUtf8Boundary(line, mid);

    if (mid <= low) {
      // Can't make progress, exit
      break;
    }

    std::string substr = line.substr(0, mid);
    int width = renderer.getTextWidth(fontId, substr.c_str());

    if (width <= maxWidth) {
      bestFit = mid;
      low = mid;
    } else {
      high = mid - 1;
      if (high > 0) {
        high = findUtf8Boundary(line, high);
      }
    }
  }

  // Try to break at word boundary (space) if possible
  if (bestFit > 0 && bestFit < line.length()) {
    size_t spacePos = line.rfind(' ', bestFit);
    if (spacePos != std::string::npos && spacePos > 0) {
      // Check if breaking at space still fits
      std::string atSpace = line.substr(0, spacePos);
      if (renderer.getTextWidth(fontId, atSpace.c_str()) <= maxWidth) {
        return spacePos;
      }
    }
  }

  return bestFit > 0 ? bestFit : 1;  // At minimum, consume 1 character
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

  backHistory.clear();
  currentPageLines.clear();
  currentPageLineEndsParagraph.clear();
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

  if (prevTriggered) {
    if (currentOffset == 0) {
      return;  // already at start
    }
    if (!backHistory.empty()) {
      currentOffset = backHistory.back();
      backHistory.pop_back();
    } else {
      currentOffset = findBackwardPageStart(currentOffset);
    }
    requestUpdate();
  } else if (nextTriggered) {
    if (currentEndOffset >= fileSize) {
      onGoHome();
      return;
    }
    // Push the page we're leaving so Back can return to it, capped so we
    // don't grow forever on very long reads.
    if (backHistory.size() >= MAX_BACK_HISTORY) {
      backHistory.erase(backHistory.begin(), backHistory.begin() + (MAX_BACK_HISTORY / 4));
    }
    backHistory.push_back(currentOffset);
    currentOffset = currentEndOffset;
    requestUpdate();
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for progress validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedLineCompression = SETTINGS.getReaderLineCompression();

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
  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  fileSize = txt->getFileSize();
  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d, file: %zu bytes", viewportWidth, viewportHeight, linesPerPage,
          fileSize);

  // Load saved offset (validates settings and resets to 0 if they changed)
  loadProgress();

  initialized = true;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines,
                                         std::vector<bool>* outEndsParagraph, size_t& nextOffset) {
  outLines.clear();
  if (outEndsParagraph) outEndsParagraph->clear();
  const size_t totalSize = fileSize ? fileSize : txt->getFileSize();

  if (offset >= totalSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, totalSize - offset);
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

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= totalSize);

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

    // Word wrap if needed - use binary search for performance with SD fonts
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // Use binary search to find break position (much faster than linear search)
      size_t breakPos = findBreakPosition(renderer, cachedFontId, line, viewportWidth);

      if (breakPos >= line.length()) {
        // Whole line fits — this is the last segment of a source line, so
        // it marks the end of a paragraph (the next line in the source
        // starts a new paragraph).
        outLines.push_back(line);
        if (outEndsParagraph) outEndsParagraph->push_back(true);
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      if (breakPos == 0) {
        breakPos = 1;  // Ensure progress
      }

      outLines.push_back(line.substr(0, breakPos));
      if (outEndsParagraph) outEndsParagraph->push_back(false);

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

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
  if (nextOffset > totalSize) {
    nextOffset = totalSize;
  }

  free(buffer);

  return !outLines.empty();
}

size_t TxtReaderActivity::snapToLineStart(size_t offset) const {
  if (offset == 0 || offset >= fileSize) return offset;

  // Look back for the nearest '\n' + 1. Read at most one chunk worth.
  const size_t scanLen = std::min(static_cast<size_t>(CHUNK_SIZE), offset);
  const size_t scanStart = offset - scanLen;

  auto* buf = static_cast<uint8_t*>(malloc(scanLen));
  if (!buf) return offset;
  if (!txt->readContent(buf, scanStart, scanLen)) {
    free(buf);
    return offset;
  }

  size_t snapped = scanStart;
  for (size_t i = scanLen; i > 0; i--) {
    if (buf[i - 1] == '\n') {
      snapped = scanStart + i;  // position right after '\n'
      break;
    }
  }
  free(buf);
  return snapped;
}

size_t TxtReaderActivity::findBackwardPageStart(size_t endOffset) const {
  // No history cache to fall back on — reconstruct the previous page by
  // walking forward from a guessed earlier position until we reach endOffset.
  // Estimate the guess window from bytes-per-page seen so far so long books
  // don't need a full scan.
  const size_t perPage = estBytesPerPage > 0 ? estBytesPerPage : 2048;
  const size_t windowBytes = perPage * 2;
  size_t scanStart = endOffset > windowBytes ? endOffset - windowBytes : 0;
  scanStart = snapToLineStart(scanStart);

  std::vector<std::string> lines;
  size_t cursor = scanStart;
  size_t lastStart = scanStart;

  // Walk forward page-by-page. The last page-start at or before endOffset
  // is our answer.
  while (cursor < endOffset) {
    size_t next = cursor;
    if (!const_cast<TxtReaderActivity*>(this)->loadPageAtOffset(cursor, lines, nullptr, next)) {
      break;
    }
    if (next <= cursor) break;
    if (next >= endOffset) {
      lastStart = cursor;
      break;
    }
    lastStart = cursor;
    cursor = next;
  }
  return lastStart;
}

int TxtReaderActivity::estimatedTotalPages() const {
  if (fileSize == 0 || estBytesPerPage == 0) return 1;
  int n = static_cast<int>((fileSize + estBytesPerPage - 1) / estBytesPerPage);
  return n > 0 ? n : 1;
}

int TxtReaderActivity::estimatedCurrentPage() const {
  if (estBytesPerPage == 0) return 1;
  return static_cast<int>(currentOffset / estBytesPerPage) + 1;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Check if font or settings changed since initialization
  if (initialized) {
    const int currentFontId = SETTINGS.getReaderFontId();
    const int currentMargin = SETTINGS.screenMargin;
    const uint8_t currentAlignment = SETTINGS.paragraphAlignment;
    const float currentLineCompression = SETTINGS.getReaderLineCompression();

    if (currentFontId != cachedFontId || currentMargin != cachedScreenMargin ||
        currentAlignment != cachedParagraphAlignment ||
        currentLineCompression != cachedLineCompression) {
      LOG_DBG("TRS", "Settings changed, reinitializing (font: %d->%d)", cachedFontId, currentFontId);
      initialized = false;
      // Keep currentOffset but snap to the previous line boundary so the
      // new layout doesn't render a partial line at the top.
      currentOffset = snapToLineStart(currentOffset);
      currentEndOffset = currentOffset;
      backHistory.clear();
      estBytesPerPage = 0;
    }
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (fileSize == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentOffset >= fileSize) currentOffset = snapToLineStart(fileSize);

  LOG_DBG("TRS", "Rendering page at offset %zu", currentOffset);

  // Load current page content and remember where the next page starts.
  size_t nextOffset = currentOffset;
  currentPageLines.clear();
  currentPageLineEndsParagraph.clear();
  loadPageAtOffset(currentOffset, currentPageLines, &currentPageLineEndsParagraph, nextOffset);
  currentEndOffset = nextOffset;

  // Seed the page-count estimate from the first page we render in this
  // session so the status bar's N/M display is usable immediately.
  if (estBytesPerPage == 0 && nextOffset > currentOffset) {
    estBytesPerPage = nextOffset - currentOffset;
  }

  LOG_DBG("TRS", "Page loaded, %d lines. Rendering...", currentPageLines.size());

  renderer.clearScreen();
  renderPage();

  LOG_DBG("TRS", "Render complete, saving progress...");

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    const size_t lineCount = currentPageLines.size();
    for (size_t i = 0; i < lineCount; i++) {
      const std::string& line = currentPageLines[i];
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        // A line is "soft-wrapped" (safe to justify by widening gaps) when it
        // didn't consume a full source line AND it isn't the last line on the
        // page (the last line may actually continue on the next page even if
        // we didn't track it as wrapped).
        const bool endsParagraph =
            i < currentPageLineEndsParagraph.size() ? currentPageLineEndsParagraph[i] : true;
        const bool isLastLineOnPage = (i + 1 == lineCount);
        const bool canJustify = !endsParagraph && !isLastLineOnPage;

        // Apply text alignment
        int8_t letterSpacing = 0;
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
            if (canJustify) {
              const int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
              const int extra = contentWidth - textWidth;
              // UTF-8 char count = bytes that aren't continuation bytes (0x80-0xBF).
              int charCount = 0;
              for (unsigned char c : line) {
                if ((c & 0xC0) != 0x80) ++charCount;
              }
              const int gaps = charCount - 1;
              if (extra > 0 && gaps > 0) {
                // drawText's letterSpacing is an int8_t; clamp to ±127. For
                // typical reading layouts the per-gap extra is single digits,
                // so the clamp only matters for pathological short lines.
                const int perGap = extra / gaps;
                letterSpacing = static_cast<int8_t>(std::min(perGap, 127));
              }
            }
            break;
        }

        if (letterSpacing > 0) {
          renderer.drawText(cachedFontId, x, y, line.c_str(), letterSpacing);
        } else {
          renderer.drawText(cachedFontId, x, y, line.c_str());
        }
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
  const float progress = fileSize > 0 ? (currentOffset * 100.0f / fileSize) : 0.0f;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, estimatedCurrentPage(), estimatedTotalPages(), title);
}

void TxtReaderActivity::saveProgress() const {
  FsFile f;
  if (!Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return;
  }
  serialization::writePod(f, PROGRESS_MAGIC);
  serialization::writePod(f, PROGRESS_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedLineCompression);
  serialization::writePod(f, static_cast<uint64_t>(currentOffset));
}

void TxtReaderActivity::loadProgress() {
  currentOffset = 0;
  currentEndOffset = 0;

  FsFile f;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return;
  }

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != PROGRESS_MAGIC) return;

  uint8_t version;
  serialization::readPod(f, version);
  if (version != PROGRESS_VERSION) return;

  uint32_t savedFileSize;
  serialization::readPod(f, savedFileSize);
  if (savedFileSize != fileSize) return;  // file changed — start over

  int32_t savedWidth;
  serialization::readPod(f, savedWidth);
  if (savedWidth != viewportWidth) return;

  int32_t savedLines;
  serialization::readPod(f, savedLines);
  if (savedLines != linesPerPage) return;

  int32_t savedFontId;
  serialization::readPod(f, savedFontId);
  if (savedFontId != cachedFontId) return;

  int32_t savedMargin;
  serialization::readPod(f, savedMargin);
  if (savedMargin != cachedScreenMargin) return;

  uint8_t savedAlignment;
  serialization::readPod(f, savedAlignment);
  if (savedAlignment != cachedParagraphAlignment) return;

  float savedCompression;
  serialization::readPod(f, savedCompression);
  if (savedCompression != cachedLineCompression) return;

  uint64_t savedOffset;
  serialization::readPod(f, savedOffset);
  if (savedOffset <= fileSize) {
    currentOffset = static_cast<size_t>(savedOffset);
    LOG_DBG("TRS", "Loaded progress: offset %zu / %zu (%.0f%%)", currentOffset, fileSize,
            fileSize ? currentOffset * 100.0f / fileSize : 0.0f);
  }
}
