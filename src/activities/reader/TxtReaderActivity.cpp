#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "TxtReaderMenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Progress file magic and version. Stores byte offset + layout settings that
// were active when the offset was saved (settings are written for forward
// compatibility / debugging only — byte offset is layout-independent so we
// preserve the user's reading position across font/margin/spacing changes).
constexpr uint32_t PROGRESS_MAGIC = 0x54585450;  // "TXTP"
constexpr uint8_t PROGRESS_VERSION = 2;          // v2 adds extraParagraphSpacing

// Auto page-turn options in pages-per-minute. Index 0 disables; the rest mirror
// the EPUB reader's choices so users see consistent values across formats.
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t PAGE_TURN_RATES_COUNT = sizeof(PAGE_TURN_RATES) / sizeof(PAGE_TURN_RATES[0]);

// Long-press multi-page jump steps. Index 0 disables (long-press behaves like
// a normal page turn); other entries become the jump distance.
constexpr int PAGE_JUMP_STEPS[] = {0, 10, 20, 50, 100};
constexpr size_t PAGE_JUMP_STEPS_COUNT = sizeof(PAGE_JUMP_STEPS) / sizeof(PAGE_JUMP_STEPS[0]);

// Held duration above which a navigation button release counts as a "jump"
// instead of a single page turn. Matches the GO_HOME_MS feel so the user only
// has one threshold to learn.
constexpr unsigned long PAGE_JUMP_HOLD_MS = 1000;

int clampPercent(int percent) {
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return percent;
}

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

  // Minimum advance is one whole UTF-8 codepoint. Splitting mid-sequence here
  // produces invalid bytes at the next page's head and desynchronizes the
  // byte-offset navigation.
  size_t firstCharEnd = 1;
  while (firstCharEnd < line.length() && (static_cast<unsigned char>(line[firstCharEnd]) & 0xC0) == 0x80) {
    firstCharEnd++;
  }

  // Binary search for the break point
  size_t low = firstCharEnd;
  size_t high = line.length();
  size_t bestFit = firstCharEnd;

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

  return bestFit;  // At minimum, consume one whole UTF-8 codepoint
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
  currentPageLineStartsParagraph.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Skip one frame after a sub-activity returns so the release event that
  // closed it doesn't fall through into navigation here.
  if (skipNextButtonCheck) {
    skipNextButtonCheck = false;
    return;
  }

  // Auto page turn handling — fire pageTurn(true) on the configured cadence
  // and let any user input cancel it.
  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }
    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Open the reader menu on Confirm release.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const float progress = fileSize > 0 ? (currentOffset * 100.0f / fileSize) : 0.0f;
    const int bookProgressPercent = clampPercent(static_cast<int>(progress + 0.5f));
    startActivityForResult(
        std::make_unique<TxtReaderMenuActivity>(renderer, mappedInput, txt ? txt->getTitle() : std::string(),
                                                estimatedCurrentPage(), estimatedTotalPages(), bookProgressPercent,
                                                SETTINGS.orientation, currentPageTurnOption, currentPageJumpOption),
        [this](const ActivityResult& result) {
          const auto& menu = std::get<MenuResult>(result.data);
          // Apply orientation, auto-turn, and page-jump even when cancelled —
          // the user cycled them inside the menu and expects them to stick.
          applyOrientation(menu.orientation);
          toggleAutoPageTurn(menu.pageTurnOption);
          currentPageJumpOption = menu.pageJumpOption;
          skipNextButtonCheck = true;
          if (!result.isCancelled) {
            onReaderMenuConfirm(static_cast<TxtReaderMenuActivity::MenuAction>(menu.action));
          } else {
            requestUpdate();
          }
        });
    return;
  }

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

  // Long-press multi-page jump on navigation buttons. When enabled, switch to
  // release-based detection so we can distinguish short tap (single page) from
  // long hold (jumpPages step). When disabled, fall through to the default
  // press-based path below to preserve original snappy feel.
  if (currentPageJumpOption > 0 && currentPageJumpOption < PAGE_JUMP_STEPS_COUNT) {
    const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (prevReleased || nextReleased) {
      const unsigned long held = mappedInput.getHeldTime();
      const int step = (held >= PAGE_JUMP_HOLD_MS) ? PAGE_JUMP_STEPS[currentPageJumpOption] : 1;
      jumpPages(nextReleased ? step : -step);
      return;
    }
    // Power button is always release-driven, so honor its configured page-turn
    // role even while we're intercepting nav-button presses.
    if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      pageTurn(true);
    }
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  pageTurn(nextTriggered);
}

void TxtReaderActivity::pageTurn(const bool isForwardTurn) {
  if (!isForwardTurn) {
    if (currentOffset == 0) {
      return;  // already at start
    }
    if (!backHistory.empty()) {
      currentOffset = backHistory.back();
      backHistory.pop_back();
    } else {
      currentOffset = findBackwardPageStart(currentOffset);
    }
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }

  if (currentEndOffset >= fileSize) {
    automaticPageTurnActive = false;
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
  lastPageTurnTime = millis();
  requestUpdate();
}

void TxtReaderActivity::jumpPages(const int deltaPages) {
  if (deltaPages == 0 || fileSize == 0) {
    return;
  }
  // Use the running per-page byte estimate so we can skip without paginating.
  // Without an estimate (very first frame) bail out instead of guessing wildly.
  const size_t perPage = estBytesPerPage > 0 ? estBytesPerPage : 0;
  if (perPage == 0) {
    pageTurn(deltaPages > 0);
    return;
  }

  size_t target;
  if (deltaPages > 0) {
    const size_t deltaBytes = perPage * static_cast<size_t>(deltaPages);
    target = (currentOffset + deltaBytes >= fileSize) ? fileSize - 1 : currentOffset + deltaBytes;
  } else {
    const size_t deltaBytes = perPage * static_cast<size_t>(-deltaPages);
    target = (currentOffset > deltaBytes) ? currentOffset - deltaBytes : 0;
  }
  target = snapToLineStart(target);

  if (target == currentOffset) {
    return;
  }
  // Only a forward jump lets Back return to the pre-jump page. On a backward
  // jump, pushing the pre-jump offset would make the next PageBack jump
  // forward — the opposite of what the user just asked for. Wipe history so
  // Back keeps moving backward page-by-page.
  if (deltaPages > 0) {
    if (backHistory.size() >= MAX_BACK_HISTORY) {
      backHistory.erase(backHistory.begin(), backHistory.begin() + (MAX_BACK_HISTORY / 4));
    }
    backHistory.push_back(currentOffset);
  } else {
    backHistory.clear();
  }
  currentOffset = target;
  currentEndOffset = target;
  lastPageTurnTime = millis();
  requestUpdate();
}

void TxtReaderActivity::jumpToPercent(int percent) {
  if (fileSize == 0) {
    return;
  }
  percent = clampPercent(percent);

  // Overflow-safe (fileSize/100)*percent + (fileSize%100)*percent/100.
  size_t target =
      (fileSize / 100) * static_cast<size_t>(percent) + (fileSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    target = fileSize - 1;
  }
  target = snapToLineStart(target);

  // Reset history so backward navigation after a jump uses backward-scan,
  // matching the EPUB jump behavior of an effective fresh location.
  backHistory.clear();
  currentOffset = target;
  currentEndOffset = target;
  lastPageTurnTime = millis();
  requestUpdate();
}

void TxtReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }
  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  // Force the layout to recompute viewport + lines per page on next render.
  initialized = false;
  currentOffset = snapToLineStart(currentOffset);
  currentEndOffset = currentOffset;
  backHistory.clear();
  estBytesPerPage = 0;
  requestUpdate();
}

void TxtReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  currentPageTurnOption = selectedPageTurnOption;
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_RATES_COUNT) {
    automaticPageTurnActive = false;
    return;
  }
  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / static_cast<unsigned long>(PAGE_TURN_RATES[selectedPageTurnOption]);
  automaticPageTurnActive = true;
  requestUpdate();
}

void TxtReaderActivity::onReaderMenuConfirm(const TxtReaderMenuActivity::MenuAction action) {
  switch (action) {
    case TxtReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const float progress = fileSize > 0 ? (currentOffset * 100.0f / fileSize) : 0.0f;
      const int initialPercent = clampPercent(static_cast<int>(progress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            skipNextButtonCheck = true;
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            } else {
              requestUpdate();
            }
          });
      break;
    }
    case TxtReaderMenuActivity::MenuAction::SCREENSHOT: {
      pendingScreenshot = true;
      requestUpdate();
      break;
    }
    case TxtReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case TxtReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case TxtReaderMenuActivity::MenuAction::PAGE_JUMP_STEP:
    case TxtReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      // Inline-cycle options are applied via menu.orientation /
      // menu.pageTurnOption / menu.pageJumpOption already consumed above.
      requestUpdate();
      break;
  }
}

void TxtReaderActivity::recomputeLayout() {
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedExtraParagraphSpacing = SETTINGS.extraParagraphSpacing;
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
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;

  // ~Half a line of space between paragraphs — matches the EPUB reader's
  // visual feel for the same setting.
  paragraphSpacingPx = cachedExtraParagraphSpacing ? lineHeight / 2 : 0;

  // Conservative upper bound. Actual lines per page is determined by
  // accumulated y in loadPageAtOffset so that paragraph spacing never causes
  // the last line to overflow the viewport.
  maxLinesPerPage = viewportHeight / lineHeight;
  if (maxLinesPerPage < 1) maxLinesPerPage = 1;

  fileSize = txt->getFileSize();
  LOG_DBG("TRS", "Viewport: %dx%d, max lines/page: %d, paragraph spacing: %dpx, file: %zu bytes", viewportWidth,
          viewportHeight, maxLinesPerPage, paragraphSpacingPx, fileSize);
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  recomputeLayout();

  // Load saved offset only the first time we open this file. Settings-driven
  // re-layouts later in the session must NOT call loadProgress — its early
  // assignment of currentOffset = 0 would discard the user's position.
  if (!progressLoaded) {
    loadProgress();
    progressLoaded = true;
  }

  initialized = true;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, bool firstLineIsParagraphStart,
                                         std::vector<std::string>& outLines, std::vector<bool>* outEndsParagraph,
                                         std::vector<bool>* outStartsParagraph, size_t& nextOffset) {
  outLines.clear();
  if (outEndsParagraph) outEndsParagraph->clear();
  if (outStartsParagraph) outStartsParagraph->clear();
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

  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;
  // Track accumulated y to enforce height-based pagination. Extra paragraph
  // spacing is added BEFORE the leading wrap of each new paragraph (except
  // the page's first line) so partial-line wraps don't push content off-screen.
  int accumulatedY = 0;
  bool isFirstSourceLineOnPage = true;

  // Helper: try to add a wrapped segment. Returns true if it fit, false if
  // adding it would overflow the viewport (caller must stop). Also caps on
  // maxLinesPerPage as a paranoia guard against degenerate font metrics.
  auto tryAddLine = [&](const std::string& seg, bool endsParagraph, bool startsParagraph,
                        bool needsExtraSpacing) -> bool {
    int linePixelHeight = lineHeight + (needsExtraSpacing ? paragraphSpacingPx : 0);
    if (accumulatedY + linePixelHeight > viewportHeight && !outLines.empty()) {
      return false;
    }
    if (static_cast<int>(outLines.size()) >= maxLinesPerPage) {
      return false;
    }
    outLines.push_back(seg);
    if (outEndsParagraph) outEndsParagraph->push_back(endsParagraph);
    if (outStartsParagraph) outStartsParagraph->push_back(startsParagraph);
    accumulatedY += linePixelHeight;
    return true;
  };

  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < maxLinesPerPage) {
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

    // The leading source line on the page is a paragraph start iff the
    // caller says so (see isOffsetAtLineStart). Every subsequent source
    // line is by definition a new paragraph (preceded by '\n').
    const bool sourceLineStartsParagraph = isFirstSourceLineOnPage ? firstLineIsParagraphStart : true;
    // Extra spacing precedes a new paragraph that is NOT the page's first.
    const bool needsExtraSpacingBefore = cachedExtraParagraphSpacing && sourceLineStartsParagraph && !outLines.empty();

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;
    bool isFirstSegmentOfSourceLine = true;
    bool extraSpacingApplied = false;

    // Word wrap if needed - use binary search for performance with SD fonts.
    // Inner-loop bound is enforced by tryAddLine (height + maxLinesPerPage cap).
    while (!line.empty()) {
      // Use binary search to find break position (much faster than linear search)
      size_t breakPos = findBreakPosition(renderer, cachedFontId, line, viewportWidth);

      const bool needsSpacing = needsExtraSpacingBefore && isFirstSegmentOfSourceLine && !extraSpacingApplied;

      if (breakPos >= line.length()) {
        // Whole line fits — this is the last segment of a source line, so
        // it marks the end of a paragraph (the next line in the source
        // starts a new paragraph).
        if (!tryAddLine(line, true, isFirstSegmentOfSourceLine && sourceLineStartsParagraph, needsSpacing)) {
          // Didn't fit — current source line stays unconsumed; stop the page.
          goto pageFull;
        }
        if (needsSpacing) extraSpacingApplied = true;
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      if (breakPos == 0) {
        breakPos = 1;  // Ensure progress
      }

      if (!tryAddLine(line.substr(0, breakPos), false, isFirstSegmentOfSourceLine && sourceLineStartsParagraph,
                      needsSpacing)) {
        goto pageFull;
      }
      if (needsSpacing) extraSpacingApplied = true;
      isFirstSegmentOfSourceLine = false;

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
    isFirstSourceLineOnPage = false;
  }
pageFull:

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

bool TxtReaderActivity::isOffsetAtLineStart(size_t offset) const {
  if (offset == 0) return true;
  uint8_t prev = 0;
  if (!txt->readContent(&prev, offset - 1, 1)) {
    // Read failure: assume paragraph start so spacing applies (visual glitch
    // is less harmful than missing the spacing on every page).
    return true;
  }
  return prev == '\n';
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
    // cursor is always at a snapped line start, so the leading source line
    // on each synthetic page is a paragraph start.
    if (!const_cast<TxtReaderActivity*>(this)->loadPageAtOffset(cursor, /*firstLineIsParagraphStart=*/true, lines,
                                                                nullptr, nullptr, next)) {
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
    const uint8_t currentExtraSpacing = SETTINGS.extraParagraphSpacing;
    const float currentLineCompression = SETTINGS.getReaderLineCompression();

    if (currentFontId != cachedFontId || currentMargin != cachedScreenMargin ||
        currentAlignment != cachedParagraphAlignment || currentExtraSpacing != cachedExtraParagraphSpacing ||
        currentLineCompression != cachedLineCompression) {
      LOG_DBG("TRS", "Settings changed, recomputing layout (font: %d->%d)", cachedFontId, currentFontId);
      // Keep currentOffset (the user's reading position), but snap to the
      // previous line boundary so the new layout doesn't render a partial
      // wrap segment at the top. Do NOT call loadProgress — its early
      // assignment of currentOffset = 0 would discard the user's position
      // every time they tweaked a setting.
      recomputeLayout();
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
  // Recover from an out-of-range offset (e.g. settings change shrinking the
  // effective reachable position). snapToLineStart(fileSize) returns fileSize
  // itself and would leave loadPageAtOffset with nothing to read, so fall back
  // to the last actual page start. The fileSize == 0 case was handled above.
  if (currentOffset >= fileSize) {
    currentOffset = findBackwardPageStart(fileSize);
    currentEndOffset = currentOffset;
  }

  LOG_DBG("TRS", "Rendering page at offset %zu", currentOffset);

  // Load current page content and remember where the next page starts.
  size_t nextOffset = currentOffset;
  currentPageLines.clear();
  currentPageLineEndsParagraph.clear();
  currentPageLineStartsParagraph.clear();
  const bool pageStartsAtLineBegin = isOffsetAtLineStart(currentOffset);
  loadPageAtOffset(currentOffset, pageStartsAtLineBegin, currentPageLines, &currentPageLineEndsParagraph,
                   &currentPageLineStartsParagraph, nextOffset);
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

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
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
      const bool endsParagraph = i < currentPageLineEndsParagraph.size() ? currentPageLineEndsParagraph[i] : true;
      const bool startsParagraph =
          i < currentPageLineStartsParagraph.size() ? currentPageLineStartsParagraph[i] : false;
      // Push y down before drawing the leading wrap of a new paragraph
      // (skip the page's first line since loadPageAtOffset already excluded it).
      if (cachedExtraParagraphSpacing && startsParagraph && i > 0) {
        y += paragraphSpacingPx;
      }
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        // A line is "soft-wrapped" (safe to justify by widening gaps) when it
        // didn't consume a full source line AND it isn't the last line on the
        // page (the last line may actually continue on the next page even if
        // we didn't track it as wrapped).
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
              const int charCount = static_cast<int>(std::count_if(
                  line.begin(), line.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
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
  if (automaticPageTurnActive && pageTurnDuration > 0) {
    // Mirror the EPUB reader: while auto-turn is on, override the title with
    // the current pages-per-minute rate so the user can verify it.
    title = std::string(tr(STR_AUTO_TURN_ENABLED)) + std::to_string(60UL * 1000UL / pageTurnDuration);
  } else if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
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
  serialization::writePod(f, static_cast<int32_t>(maxLinesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedExtraParagraphSpacing);
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

  // Read remaining layout fields purely for forward-compat / debugging. We
  // intentionally do NOT validate them against current settings: byte offset
  // is layout-independent (any layout change just re-paginates around the
  // saved position), and validating would lose the user's reading position
  // every time they tweaked a font/margin/spacing setting.
  int32_t savedWidth, savedMaxLines, savedFontId, savedMargin;
  serialization::readPod(f, savedWidth);
  serialization::readPod(f, savedMaxLines);
  serialization::readPod(f, savedFontId);
  serialization::readPod(f, savedMargin);
  uint8_t savedAlignment, savedExtraSpacing;
  serialization::readPod(f, savedAlignment);
  serialization::readPod(f, savedExtraSpacing);
  float savedCompression;
  serialization::readPod(f, savedCompression);
  (void)savedWidth;
  (void)savedMaxLines;
  (void)savedFontId;
  (void)savedMargin;
  (void)savedAlignment;
  (void)savedExtraSpacing;
  (void)savedCompression;

  uint64_t savedOffset;
  serialization::readPod(f, savedOffset);
  if (savedOffset < fileSize) {
    // Snap to a line boundary so the new layout doesn't render a partial
    // wrap segment at the top of the page.
    currentOffset = snapToLineStart(static_cast<size_t>(savedOffset));
    LOG_DBG("TRS", "Loaded progress: offset %zu / %zu (%.0f%%)", currentOffset, fileSize,
            fileSize ? currentOffset * 100.0f / fileSize : 0.0f);
  }
}
