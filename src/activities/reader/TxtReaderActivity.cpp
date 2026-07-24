#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <BufferedFile.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ProgressFileCodec.h"
#include "ReaderUtils.h"
#include "ReadingStatsActivity.h"
#include "ReadingStatsDateEditActivity.h"
#include "ReadingStatsCompletionTransaction.h"
#include "RecentBooksStore.h"
#include "PerBookReaderSettingsBridge.h"
#include "PerBookReaderSettingsStore.h"
#include "SdCardFontSystem.h"
#include "TxtLineWrap.h"
#include "activities/reader/BookReaderSettingsActivity.h"
#include "activities/reader/ClipSelectionActivity.h"
#include "activities/reader/ClippingListActivity.h"
#include "activities/reader/DictionaryWordSelectActivity.h"
#include "activities/reader/EpubReaderBookmarksActivity.h"
#include "activities/reader/EpubReaderPercentSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookmarkUtil.h"
#include "util/DictionaryHistoryStore.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// The legacy scan is cheaper when a short line only barely overflows.
constexpr size_t MIN_BINARY_WRAP_BYTES = 96;
constexpr size_t CACHE_IO_BUFFER_SIZE = 1024;
constexpr size_t CACHE_HEADER_SIZE = 34;
constexpr size_t INITIAL_INDEX_CAPACITY = 256;
// ESP32-C3 has no PSRAM. Keep a corrupt or pathological text file from growing
// the in-memory offset table until std::vector aborts (firmware builds disable
// exceptions). 16K pages still exceeds any practical plain-text book while
// bounding the table itself to 64 KiB on-device.
constexpr size_t MAX_INDEX_PAGES = 16U * 1024U;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 5;          // Increment when pagination behavior changes
constexpr uint8_t MIN_AUTO_PAGE_TURN_SECONDS = 5;
constexpr uint8_t MAX_AUTO_PAGE_TURN_SECONDS = 120;
constexpr unsigned long MILLISECONDS_PER_SECOND = 1000UL;

uint8_t normalizeAutoPageTurnSeconds(const uint8_t seconds) {
  return seconds == 0 ? 0 : std::clamp<uint8_t>(seconds, MIN_AUTO_PAGE_TURN_SECONDS, MAX_AUTO_PAGE_TURN_SECONDS);
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  autoPageTurnSeconds = normalizeAutoPageTurnSeconds(
      bookReaderSettings.hasAutoPageTurnInterval ? bookReaderSettings.autoPageTurnSeconds : 0);
  automaticPageTurnActive = bookReaderSettings.autoPageTurnStartsOnOpen && autoPageTurnSeconds != 0;
  lastPageTurnTime = millis();
  confirmHold.reset();
  pageTurnGesture.reset();
  ignoreNextConfirmRelease = false;

  txt->setupCacheDir();

  const ClippingStore::LoadResult clippingLoad =
      clippingStore.loadForBook(txt->getPath(), txt->getTitle(), "", "txt");
  if (!clippingStore.isLoaded()) {
    LOG_ERR("TRS", "TXT clipping store unavailable (status %u)", static_cast<unsigned>(clippingLoad));
  }

  const ReadingStatsCompletionTransaction::RecoveryResult completionRecovery =
      ReadingStatsCompletionTransaction::recoverPending();
  const bool completionStatsWritable = completionRecovery != ReadingStatsCompletionTransaction::RecoveryResult::Blocked;

  BookReadingStats::LoadStatus bookStatsStatus = BookReadingStats::LoadStatus::Missing;
  bookReadingStats = BookReadingStats::load(txt->getCachePath(), &bookStatsStatus);
  bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(bookStatsStatus);
  bookReadingStatsWritable =
      completionStatsWritable && bookReadingStatsTrusted && BookReadingStats::canPublish(txt->getCachePath());
  GlobalReadingStats::LoadStatus globalStatsStatus = GlobalReadingStats::LoadStatus::Missing;
  globalReadingStats = GlobalReadingStats::load(&globalStatsStatus);
  globalReadingStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(globalStatsStatus);
  globalReadingStatsWritable = completionStatsWritable && globalReadingStatsTrusted;
  readingSessionTracker = ReadingSessionTracker{};
  sessionReadingSeconds = 0;
  pendingBookReadingSpans = {};
  pendingGlobalReadingSpans = {};
  hasActiveReadingSpanStartLocalDateTime = false;
  hasSessionStartLocalDateTime = false;
  readingSessionCommitted = false;
  bookReadingStatsDirty = false;
  globalReadingStatsDirty = false;
  completionAttemptBlocked = false;
  pendingStatsCompletionError = false;
  pendingReadingViewSignal.store(0, std::memory_order_relaxed);

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

  commitReadingSession();
  saveReadingStats();
  DICTIONARY_HISTORY.flush();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  applyReaderSettings(globalReaderSettings);
  sdFontSystem.ensureLoaded(renderer);

  pageOffsets.reset();
  pageOffsetCount = 0;
  pageOffsetCapacity = 0;
  currentPageLines.clear();
  currentPageLineOffsets.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
  clippingStore.unload();
}

void TxtReaderActivity::onPause() {
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  lastPageTurnTime = millis();
}

void TxtReaderActivity::onResume() {
  // A child activity owns the panel until this reader successfully redraws.
  pendingReadingViewSignal.store(0, std::memory_order_release);
  lastPageTurnTime = millis();
  pageTurnGesture.reset();
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmHold.reset();
    ignoreNextConfirmRelease = false;
  }
}

void TxtReaderActivity::loop() {
  consumeReadingViewSignal();
  if (showBookmarkMessage && millis() - bookmarkMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }
  if (showDictionaryMessage && millis() - dictionaryMessageTime >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }
  if (readingSessionTracker.discardIfIdle(static_cast<uint32_t>(millis()))) {
    hasActiveReadingSpanStartLocalDateTime = false;
    LOG_DBG("TRS", "Reading interval discarded after idle threshold");
  }

  if (!txt) {
    finish();
    return;
  }

  const bool readerReady = initialized.load(std::memory_order_acquire) && !initializationFailed && pageOffsetCount > 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHold.onPress();
  const bool suppressConfirmRelease =
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) && ignoreNextConfirmRelease;
  if (suppressConfirmRelease) {
    confirmHold.onRelease();
    ignoreNextConfirmRelease = false;
  }

  if (automaticPageTurnActive) {
    if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !suppressConfirmRelease) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    if (readerReady && !RenderLock::peek() &&
        millis() - lastPageTurnTime >= static_cast<unsigned long>(autoPageTurnSeconds) * MILLISECONDS_PER_SECOND) {
      if (currentPage < totalPages - 1) {
        consumeReadingViewSignal();
        stopReadingPage(true, static_cast<uint32_t>(millis()));
        ++currentPage;
        lastPageTurnTime = millis();
        requestUpdate();
      } else {
        automaticPageTurnActive = false;
      }
      return;
    }
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  if (readerReady && SETTINGS.longPressMenuFunction != CrossPointSettings::LP_MENU_DISABLED &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      confirmHold.onHold(mappedInput.getHeldTime(), ReaderUtils::CONFIRM_HOLD_MS)) {
    ignoreNextConfirmRelease = true;
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        loadCachedBookmarks();
        updateCurrentPageBookmarked();
        showBookmarkMessage = toggleBookmark();
        if (showBookmarkMessage) bookmarkMessageTime = millis();
        requestUpdate();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_READING_STATS:
        openReadingStats();
        return;
      case CrossPointSettings::LP_MENU_AUTO_PAGE_TURN:
        autoPageTurnSeconds = ReaderUtils::autoPageTurnShortcutSeconds(autoPageTurnSeconds);
        automaticPageTurnActive = !automaticPageTurnActive;
        lastPageTurnTime = millis();
        requestUpdate();
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
      default:
        pendingShortcutUnsupportedNotice = true;
        requestUpdate();
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
        break;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (suppressConfirmRelease) return;
    if (readerReady && confirmHold.onRelease() == ReaderUtils::HoldRelease::Short) {
      openReaderMenu();
    }
    return;
  }
  // Ignore page input while the render task is still indexing. In particular,
  // totalPages starts at one and must not be mistaken for a one-page book.
  if (!readerReady) return;

  const auto pageGesture = ReaderUtils::detectPageTurnGesture(mappedInput, pageTurnGesture);
  const bool prevTriggered = pageGesture.prev;
  const bool nextTriggered = pageGesture.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (pageGesture.longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    pageTurnGesture.reset();
    applyOrientation(newOrientation);
    return;
  }

  const int pageDelta = pageGesture.longPress ? 10 : 1;

  if (prevTriggered) {
    bool changed = false;
    {
      RenderLock lock(*this);
      consumeReadingViewSignal();
      if (currentPage > 0) {
        stopReadingPage(false, static_cast<uint32_t>(millis()));
        currentPage = std::max(0, currentPage - pageDelta);
        completionAttemptBlocked = false;
        changed = true;
      }
    }
    if (changed) requestUpdate();
  } else if (nextTriggered) {
    bool changed = false;
    bool goHome = false;
    bool completionFailed = false;
    {
      RenderLock lock(*this);
      consumeReadingViewSignal();
      stopReadingPage(true, static_cast<uint32_t>(millis()));
      if (currentPage < totalPages - 1) {
        currentPage = std::min(totalPages - 1, currentPage + pageDelta);
        changed = true;
      } else if (lastSuccessfullyRenderedPage != currentPage) {
        // Button notifications can arrive faster than e-paper renders. Do not
        // complete a book until its actual last page has reached the panel.
        changed = true;
      } else {
        markBookCompleted();
        // Surface a protected/corrupt-store failure once before allowing the
        // next press (or Back) to leave the book.
        completionFailed = pendingStatsCompletionError;
        goHome = !completionFailed;
      }
    }
    if (completionFailed) return;
    if (goHome) {
      onGoHome();
    } else if (changed) {
      requestUpdate();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized.load(std::memory_order_acquire)) {
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
  cachedLineAdvance = std::max(
      1, static_cast<int>(renderer.getLineHeight(cachedFontId) * SETTINGS.getReaderLineCompression() + 0.5f));

  linesPerPage = viewportHeight / cachedLineAdvance;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    const bool indexComplete = buildPageIndex();
    if (indexComplete && pageOffsetCount > 0) {
      // Save only complete indexes. A transient read/allocation failure must
      // not persist a permanently truncated view of the book.
      savePageIndexCache();
    } else if (!indexComplete) {
      // A partial index must never be rendered or mistaken for the end of the
      // book. The next open can retry from the unchanged source file.
      pageOffsets.reset();
      pageOffsetCount = 0;
      pageOffsetCapacity = 0;
      totalPages = 0;
      initializationFailed = true;
    }
  }

  if (!initializationFailed) {
    loadProgress();
    if (initialClippingJump) {
      if (validateClippingJump(*initialClippingJump) && initialClippingJump->hasTextAnchor &&
          initialClippingJump->textSourceStart < txt->getFileSize()) {
        jumpToByteOffset(initialClippingJump->textSourceStart);
      } else {
        pendingClippingNotice = ClippingNotice::JumpUnavailable;
      }
      initialClippingJump.reset();
    }
  }

  initialized.store(true, std::memory_order_release);
}

bool TxtReaderActivity::buildPageIndex() {
  pageOffsets.reset();
  pageOffsetCount = 0;
  pageOffsetCapacity = 0;
  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    totalPages = 0;
    return true;
  }

  if (!appendPageOffset(0)) return false;  // First page starts at offset 0

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
    if (offset < fileSize && !appendPageOffset(static_cast<uint32_t>(offset))) break;

    // Yield to other tasks periodically
    if (pageOffsetCount % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = static_cast<int>(pageOffsetCount);
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
  return offset >= fileSize;
}

bool TxtReaderActivity::appendPageOffset(const uint32_t offset) {
  if (pageOffsetCount >= MAX_INDEX_PAGES) {
    LOG_ERR("TRS", "TXT page index exceeds the safe %zu-page limit", MAX_INDEX_PAGES);
    return false;
  }
  if (pageOffsetCount == pageOffsetCapacity) {
    const size_t nextCapacity =
        pageOffsetCapacity == 0 ? INITIAL_INDEX_CAPACITY : std::min(pageOffsetCapacity * 2, MAX_INDEX_PAGES);
    auto grown = makeUniqueNoThrow<uint32_t[]>(nextCapacity);
    if (!grown) {
      LOG_ERR("TRS", "Could not grow TXT page index to %zu entries", nextCapacity);
      return false;
    }
    if (pageOffsetCount > 0) std::copy_n(pageOffsets.get(), pageOffsetCount, grown.get());
    pageOffsets = std::move(grown);
    pageOffsetCapacity = nextCapacity;
  }
  pageOffsets[pageOffsetCount++] = offset;
  return true;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                                         std::vector<uint32_t>* outLineOffsets) {
  outLines.clear();
  if (outLineOffsets) outLineOffsets->clear();
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
        if (outLineOffsets) outLineOffsets->push_back(static_cast<uint32_t>(offset + pos + lineBytePos));
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        if (outLineOffsets) outLineOffsets->push_back(static_cast<uint32_t>(offset + pos + lineBytePos));
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

      if (outLineOffsets) outLineOffsets->push_back(static_cast<uint32_t>(offset + pos + lineBytePos));
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

std::unique_ptr<Page> TxtReaderActivity::buildInteractivePage(const uint16_t pageIndex,
                                                              std::vector<TextWordAnchor>* anchors) {
  if (!txt || static_cast<size_t>(pageIndex) >= pageOffsetCount) return {};
  std::vector<std::string> lines;
  std::vector<uint32_t> lineOffsets;
  size_t nextOffset = 0;
  if (!loadPageAtOffset(pageOffsets[pageIndex], lines, nextOffset, &lineOffsets)) return {};

  auto page = std::make_unique<Page>();
  if (anchors) anchors->clear();
  int y = cachedOrientedMarginTop;
  for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex, y += cachedLineAdvance) {
    const std::string& line = lines[lineIndex];
    if (line.empty()) continue;

    std::vector<std::string> words;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (size_t cursor = 0; cursor < line.size();) {
      while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) ++cursor;
      const size_t start = cursor;
      while (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor]))) ++cursor;
      if (start < cursor) {
        words.emplace_back(line.substr(start, cursor - start));
        ranges.emplace_back(start, cursor);
      }
    }
    if (words.empty()) continue;

    const bool rtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
    uint8_t effectiveAlignment = cachedParagraphAlignment;
    if (rtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
      effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
    }
    const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);
    int lineX = cachedOrientedMarginLeft;
    if (effectiveAlignment == CrossPointSettings::CENTER_ALIGN) {
      lineX += (viewportWidth - textWidth) / 2;
    } else if (effectiveAlignment == CrossPointSettings::RIGHT_ALIGN) {
      lineX += viewportWidth - textWidth;
    }

    std::vector<int16_t> positions(words.size());
    std::vector<uint16_t> visualOrder;
    const bool reordered = BidiUtils::computeVisualWordOrder(words, rtl, visualOrder);
    if (rtl && reordered) {
      int cursorX = 0;
      const int spaceWidth = renderer.getTextAdvanceX(cachedFontId, " ", EpdFontFamily::REGULAR);
      for (const uint16_t wordIndex : visualOrder) {
        positions[wordIndex] = static_cast<int16_t>(cursorX);
        cursorX += renderer.getTextAdvanceX(cachedFontId, words[wordIndex].c_str(), EpdFontFamily::REGULAR) +
                   spaceWidth;
      }
    } else {
      for (size_t index = 0; index < words.size(); ++index) {
        positions[index] = static_cast<int16_t>(renderer.getTextAdvanceX(
            cachedFontId, line.substr(0, ranges[index].first).c_str(), EpdFontFamily::REGULAR));
      }
    }

    std::vector<EpdFontFamily::Style> styles(words.size(), EpdFontFamily::REGULAR);
    BlockStyle blockStyle;
    blockStyle.isRtl = rtl;
    blockStyle.directionDefined = rtl;
    auto block = std::make_shared<TextBlock>(words, positions, styles, std::vector<uint8_t>{},
                                             std::vector<uint16_t>{}, blockStyle);
    if (!block->valid()) return {};
    page->elements.push_back(std::make_shared<PageLine>(std::move(block), static_cast<int16_t>(lineX),
                                                        static_cast<int16_t>(y)));
    if (anchors && lineIndex < lineOffsets.size()) {
      for (const auto& [start, end] : ranges) {
        anchors->push_back({static_cast<uint32_t>(lineOffsets[lineIndex] + start),
                            static_cast<uint32_t>(lineOffsets[lineIndex] + end)});
      }
    }
  }
  return page;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    lastSuccessfullyRenderedPage = -1;
    signalReadingPageHidden();
    return;
  }

  // Initialize reader if not done
  if (!initialized.load(std::memory_order_acquire)) {
    initializeReader();
  }

  if (initializationFailed) {
    lastSuccessfullyRenderedPage = -1;
    signalReadingPageHidden();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (pageOffsetCount == 0) {
    lastSuccessfullyRenderedPage = -1;
    signalReadingPageHidden();
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
  if (!loadPageAtOffset(offset, currentPageLines, nextOffset, &currentPageLineOffsets)) {
    lastSuccessfullyRenderedPage = -1;
    signalReadingPageHidden();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  renderPage();
  lastSuccessfullyRenderedPage = currentPage;

  if (currentPage != lastSavedPage && saveProgress()) {
    lastSavedPage = currentPage;
  }

  if (pendingStatsCompletionError) {
    pendingStatsCompletionError = false;
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_COMPLETE_BOOK_STATS_FAILED));
  } else if (pendingBookSettingsSaveError) {
    pendingBookSettingsSaveError = false;
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_SAVE_BOOK_SETTINGS_FAILED));
  } else if (pendingCacheClearError) {
    pendingCacheClearError = false;
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_CLEAR_CACHE_FAILED));
  } else if (pendingShortcutUnsupportedNotice) {
    pendingShortcutUnsupportedNotice = false;
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_SHORTCUT_NOT_SUPPORTED));
  } else if (pendingBookmarkStorageError) {
    pendingBookmarkStorageError = false;
    signalReadingPageHidden();
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  } else if (pendingClippingNotice != ClippingNotice::None) {
    const ClippingNotice notice = pendingClippingNotice;
    pendingClippingNotice = ClippingNotice::None;
    signalReadingPageHidden();
    switch (notice) {
      case ClippingNotice::Saved:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_SAVED));
        break;
      case ClippingNotice::LimitReached:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_LIMIT_REACHED));
        break;
      case ClippingNotice::SaveFailed:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_SAVE_FAILED));
        break;
      case ClippingNotice::NewerFormat:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_NEWER_FORMAT));
        break;
      case ClippingNotice::JumpUnavailable:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_JUMP_UNAVAILABLE));
        break;
      case ClippingNotice::Unavailable:
        GUI.drawPopup(renderer, tr(STR_CLIPPING_UNAVAILABLE));
        break;
      case ClippingNotice::None:
        break;
    }
  } else {
    signalReadingPageVisible();
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
  if (showDictionaryMessage) GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  lastPageTurnTime = millis();
}

void TxtReaderActivity::renderPage() {
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
      y += cachedLineAdvance;
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

void TxtReaderActivity::signalReadingPageVisible() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(1, std::memory_order_release);
}

void TxtReaderActivity::signalReadingPageHidden() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(-1, std::memory_order_release);
}

void TxtReaderActivity::consumeReadingViewSignal() {
  const int8_t signal = pendingReadingViewSignal.exchange(0, std::memory_order_acq_rel);
  if (signal == 0) return;

  const uint32_t eventAtMs = pendingReadingViewAtMs.load(std::memory_order_relaxed);
  if (signal > 0) {
    if (readingSessionTracker.pageVisible(eventAtMs)) {
      ReadingStatsDateTime localStart;
      hasActiveReadingSpanStartLocalDateTime = getCurrentLocalReadingStatsDateTime(localStart);
      if (hasActiveReadingSpanStartLocalDateTime) {
        activeReadingSpanStartLocalDateTime = localStart;
        if (!hasSessionStartLocalDateTime) {
          sessionStartLocalDateTime = localStart;
          hasSessionStartLocalDateTime = true;
        }
      }
    }
  } else {
    stopReadingPage(false, eventAtMs);
  }
}

void TxtReaderActivity::recordReadingSample(const ReadingSessionSample& sample) {
  if (sample.seconds > 0) {
    sessionReadingSeconds = addReadingStatsSaturated(sessionReadingSeconds, sample.seconds);
  }
  if (!sample.forwardPageRead) return;

  if (bookReadingStatsWritable) {
    bookReadingStats.totalPagesTurned = addReadingStatsSaturated(bookReadingStats.totalPagesTurned, 1);
    bookReadingStatsDirty = true;
  }
  if (globalReadingStatsWritable) {
    globalReadingStats.totalPagesTurned = addReadingStatsSaturated(globalReadingStats.totalPagesTurned, 1);
    globalReadingStatsDirty = true;
  }
}

void TxtReaderActivity::stopReadingPage(const bool forwardPageTurn, const uint32_t nowMs) {
  const ReadingSessionSample sample = readingSessionTracker.stop(nowMs, forwardPageTurn);
  if (sample.seconds > 0 && hasActiveReadingSpanStartLocalDateTime) {
    pendingBookReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
    pendingGlobalReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
  }
  hasActiveReadingSpanStartLocalDateTime = false;
  recordReadingSample(sample);
}

void TxtReaderActivity::commitReadingSession() {
  if (readingSessionCommitted) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  readingSessionCommitted = true;

  if (sessionReadingSeconds >= 60) {
    if (bookReadingStatsWritable) {
      if (bookReadingStats.sessionCount < std::numeric_limits<uint16_t>::max()) {
        ++bookReadingStats.sessionCount;
      }
      bookReadingStatsDirty = true;
    }
    if (globalReadingStatsWritable) {
      globalReadingStats.totalSessions = addReadingStatsSaturated(globalReadingStats.totalSessions, 1);
      if (hasSessionStartLocalDateTime) globalReadingStats.recordReadingSession(sessionStartLocalDateTime.date);
      globalReadingStatsDirty = true;
    }
  }

  if (sessionReadingSeconds < 10) return;

  if (bookReadingStatsWritable) {
    bookReadingStats.totalReadingSeconds =
        addReadingStatsSaturated(bookReadingStats.totalReadingSeconds, sessionReadingSeconds);
    for (size_t i = 0; i < bookReadingStats.timeOfDaySeconds.size(); ++i) {
      bookReadingStats.timeOfDaySeconds[i] =
          addReadingStatsSaturated(bookReadingStats.timeOfDaySeconds[i], pendingBookReadingSpans.timeOfDaySeconds[i]);
    }
    for (size_t i = 0; i < bookReadingStats.dayOfWeekSeconds.size(); ++i) {
      bookReadingStats.dayOfWeekSeconds[i] =
          addReadingStatsSaturated(bookReadingStats.dayOfWeekSeconds[i], pendingBookReadingSpans.dayOfWeekSeconds[i]);
    }
    if (sessionReadingSeconds >= 120 && hasSessionStartLocalDateTime && !bookReadingStats.startDateManual &&
        !bookReadingStats.startDate.isValid()) {
      bookReadingStats.startDate = sessionStartLocalDateTime.date;
      bookReadingStats.startMinuteOfDay =
          static_cast<uint16_t>(sessionStartLocalDateTime.hour) * 60u + sessionStartLocalDateTime.minute;
    }
    bookReadingStatsDirty = true;
  }
  if (globalReadingStatsWritable) {
    globalReadingStats.totalReadingSeconds =
        addReadingStatsSaturated(globalReadingStats.totalReadingSeconds, sessionReadingSeconds);
    globalReadingStats.merge(pendingGlobalReadingSpans);
    globalReadingStats.longestReadingStreak = globalReadingStats.displayLongestReadingStreak();
    globalReadingStatsDirty = true;
  }
}

void TxtReaderActivity::saveReadingStats() {
  if (bookReadingStatsWritable && bookReadingStatsDirty && txt) {
    if (bookReadingStats.save(txt->getCachePath())) {
      bookReadingStatsDirty = false;
    } else {
      LOG_ERR("TRS", "Failed to save book reading statistics");
    }
  }
  if (globalReadingStatsWritable && globalReadingStatsDirty) {
    if (globalReadingStats.save()) {
      globalReadingStatsDirty = false;
    } else {
      LOG_ERR("TRS", "Failed to save global reading statistics");
    }
  }
}

void TxtReaderActivity::markBookCompleted() {
  if (!txt || bookReadingStats.isCompleted || completionAttemptBlocked) return;
  const auto reportFailure = [this]() {
    completionAttemptBlocked = true;
    pendingStatsCompletionError = true;
    requestUpdate();
  };
  if (!bookReadingStatsWritable || !globalReadingStatsWritable) {
    LOG_ERR("TRS", "Could not mark the book complete because reading statistics are protected or unreadable");
    reportFailure();
    return;
  }

  saveReadingStats();
  if (bookReadingStatsDirty || globalReadingStatsDirty) {
    LOG_ERR("TRS", "Could not flush reading statistics before marking the book complete");
    reportFailure();
    return;
  }

  BookReadingStats completedBookStats = bookReadingStats;
  GlobalReadingStats completedGlobalStats = globalReadingStats;
  completedBookStats.isCompleted = true;
  completedBookStats.estimatedTimeLeftSeconds = 0;
  if (!completedBookStats.finishedDateManual && !completedBookStats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      completedBookStats.finishedDate = now.date;
      completedBookStats.finishedMinuteOfDay = static_cast<uint16_t>(now.hour) * 60u + now.minute;
    }
  }
  completedGlobalStats.completedBooks = addReadingStatsSaturated(completedGlobalStats.completedBooks, 1);
  if (!ReadingStatsCompletionTransaction::commit(txt->getCachePath(), bookReadingStats, completedBookStats,
                                                 globalReadingStats, completedGlobalStats)) {
    LOG_ERR("TRS", "Could not commit book completion statistics");
    bookReadingStatsWritable = false;
    globalReadingStatsWritable = false;
    reportFailure();
    return;
  }
  bookReadingStats = completedBookStats;
  globalReadingStats = completedGlobalStats;
}

void TxtReaderActivity::openReaderMenu() {
  if (!txt || pageOffsetCount == 0) return;

  loadCachedBookmarks();
  updateCurrentPageBookmarked();
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  const int displayedPage = lastSuccessfullyRenderedPage >= 0 ? lastSuccessfullyRenderedPage : currentPage;
  int progressPercent = 0;
  if (txt->getFileSize() > 0 && displayedPage >= 0 && static_cast<size_t>(displayedPage) < pageOffsetCount) {
    const size_t completedOffset = static_cast<size_t>(displayedPage + 1) < pageOffsetCount
                                       ? pageOffsets[displayedPage + 1]
                                       : txt->getFileSize();
    progressPercent = static_cast<int>(std::min<uint64_t>(
        100, static_cast<uint64_t>(completedOffset) * 100 / static_cast<uint64_t>(txt->getFileSize())));
  }

  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, txt->getTitle(), displayedPage + 1, totalPages, progressPercent, SETTINGS.orientation,
          autoPageTurnSeconds, automaticPageTurnActive, false, !cachedBookmarks.empty(), currentPageBookmarked,
          EpubReaderMenuActivity::ReaderKind::PlainText, clippingStore.isLoaded(),
          clippingStore.isLoaded() && clippingStore.size() > 0),
      [this](const ActivityResult& result) {
        const auto* menu = std::get_if<MenuResult>(&result.data);
        if (!menu) {
          requestUpdate();
          return;
        }
        applyOrientation(menu->orientation);
        if (menu->autoPageTurnChanged) updateAutoPageTurnFromMenu(menu->autoPageTurnSeconds);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu->action));
        } else {
          requestUpdate();
        }
      });
}

void TxtReaderActivity::onReaderMenuConfirm(const EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      int initialPercent = 0;
      if (txt && txt->getFileSize() > 0 && currentPage >= 0 && static_cast<size_t>(currentPage) < pageOffsetCount) {
        initialPercent = static_cast<int>(std::min<uint64_t>(
            100, static_cast<uint64_t>(pageOffsets[currentPage]) * 100 / static_cast<uint64_t>(txt->getFileSize())));
      }
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) jumpToPercent(std::get<PercentResult>(result.data).percent);
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS:
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, txt->getPath()),
          [this](const ActivityResult& result) {
            loadCachedBookmarks();
            if (!result.isCancelled) {
              const auto* progress = std::get_if<ProgressChangeResult>(&result.data);
              if (progress && progress->hasTextByteOffset) jumpToByteOffset(progress->textByteOffset);
            }
            requestUpdate();
          });
      break;
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK:
      showBookmarkMessage = toggleBookmark();
      if (showBookmarkMessage) bookmarkMessageTime = millis();
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::BOOK_SETTINGS:
      openBookReaderSettings();
      break;
    case EpubReaderMenuActivity::MenuAction::DICTIONARY:
      openDictionaryWordSelect();
      break;
    case EpubReaderMenuActivity::MenuAction::READING_STATS:
      openReadingStats();
      break;
    case EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING:
      openClippingSelection();
      break;
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS:
      openClippings();
      break;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      break;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE:
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_CLEAR_READING_CACHE), ""),
          [this](const ActivityResult& result) {
            if (result.isCancelled || !txt) {
              requestUpdate();
              return;
            }
            if (!saveProgress() || !clearBookCacheDirectoryPreservingUserState(txt->getCachePath())) {
              pendingCacheClearError = true;
              recoverBookCacheUserState(txt->getCachePath(), txt->getPath());
              requestUpdate();
              return;
            }
            txt->setupCacheDir();
            invalidateReaderLayout();
            requestUpdate();
          });
      break;
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case EpubReaderMenuActivity::MenuAction::GO_TO_PAGE:
    case EpubReaderMenuActivity::MenuAction::MARK_COMPLETE:
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER:
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES:
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR:
    case EpubReaderMenuActivity::MenuAction::SYNC:
    case EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC:
      requestUpdate();
      break;
  }
}

void TxtReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (currentPage < 0 || static_cast<size_t>(currentPage) >= pageOffsetCount) return;
  auto page = buildInteractivePage(static_cast<uint16_t>(currentPage));
  if (!page) {
    requestUpdate();
    return;
  }
  startActivityForResult(
      std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page), 0, 0),
      [this](const ActivityResult&) { requestUpdate(); });
}

void TxtReaderActivity::openClippingSelection() {
  if (!txt || !clippingStore.isLoaded() || currentPage < 0 || static_cast<size_t>(currentPage) >= pageOffsetCount) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }
  auto page = buildInteractivePage(static_cast<uint16_t>(currentPage));
  if (!page) {
    pendingClippingNotice = ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }
  const ClipSelectionActivity::PageLoader loader{
      this, [](void* context, const uint16_t pageIndex) -> std::unique_ptr<Page> {
        return static_cast<TxtReaderActivity*>(context)->buildInteractivePage(pageIndex);
      }};
  startActivityForResult(
      std::make_unique<ClipSelectionActivity>(renderer, mappedInput, std::move(page), SETTINGS.getReaderFontId(), 0,
                                              0, static_cast<uint16_t>(currentPage),
                                              static_cast<uint16_t>(totalPages), UINT16_MAX, loader),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* selection = std::get_if<ClippingSelectionResult>(&result.data);
        if (!selection || !txt || !clippingStore.isLoaded()) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }

        std::vector<TextWordAnchor> startAnchors;
        std::vector<TextWordAnchor> endAnchors;
        if (!buildInteractivePage(selection->startPage, &startAnchors) ||
            selection->startPageWordIndex >= startAnchors.size()) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }
        if (selection->endPage == selection->startPage) {
          endAnchors = startAnchors;
        } else if (!buildInteractivePage(selection->endPage, &endAnchors)) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }
        if (selection->endPageWordIndex >= endAnchors.size()) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }
        const uint32_t sourceStart = startAnchors[selection->startPageWordIndex].start;
        const uint32_t sourceEnd = endAnchors[selection->endPageWordIndex].end;
        if (sourceStart >= sourceEnd || sourceEnd > txt->getFileSize()) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }

        ClippingCodec::ClippingMetadata clipping;
        clipping.startPage = selection->startPage;
        clipping.endPage = selection->endPage;
        clipping.pageCount = selection->pageCount;
        clipping.startWordIndex = selection->startPageWordIndex;
        clipping.endWordIndex = selection->endPageWordIndex;
        clipping.wordCount = selection->wordCount;
        clipping.paragraphIndex = UINT16_MAX;
        clipping.pageFingerprint = 0;
        clipping.hasTextAnchor = true;
        clipping.textSourceStart = sourceStart;
        clipping.textSourceEnd = sourceEnd;
        const std::time_t now = std::time(nullptr);
        if (now >= 1577836800 && static_cast<uint64_t>(now) <= UINT32_MAX) {
          clipping.timestamp = static_cast<uint32_t>(now);
        }
        switch (clippingStore.add(clipping, selection->text)) {
          case ClippingStore::AddResult::Added:
            pendingClippingNotice = ClippingNotice::Saved;
            break;
          case ClippingStore::AddResult::LimitReached:
            pendingClippingNotice = ClippingNotice::LimitReached;
            break;
          case ClippingStore::AddResult::InvalidData:
          case ClippingStore::AddResult::SaveFailed:
            pendingClippingNotice = ClippingNotice::SaveFailed;
            break;
        }
        requestUpdate();
      });
}

void TxtReaderActivity::openClippings() {
  if (!txt || !clippingStore.isLoaded()) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }
  startActivityForResult(std::make_unique<ClippingListActivity>(renderer, mappedInput, clippingStore),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto* jump = std::get_if<ClippingJumpResult>(&result.data);
                             if (jump && validateClippingJump(*jump)) {
                               jumpToByteOffset(jump->textSourceStart);
                             } else {
                               pendingClippingNotice = ClippingNotice::JumpUnavailable;
                             }
                           }
                           requestUpdate();
                         });
}

bool TxtReaderActivity::validateClippingJump(const ClippingJumpResult& jump) const {
  if (!txt || !clippingStore.isLoaded() || jump.clippingIndex >= clippingStore.size() ||
      jump.bookPath != txt->getPath() || jump.bookType != "txt" || !jump.hasTextAnchor ||
      jump.textSourceStart >= jump.textSourceEnd || jump.textSourceEnd > txt->getFileSize()) {
    return false;
  }
  const std::string canonical = ClippingCodec::filePathForBook(jump.bookPath, jump.bookType);
  if (canonical.empty() || canonical != jump.storePath || canonical != clippingStore.path() ||
      jump.storeFileLength != clippingStore.fileLength() ||
      jump.storeFormat != static_cast<uint8_t>(clippingStore.format())) {
    return false;
  }
  const auto* clipping = clippingStore.at(jump.clippingIndex);
  if (!clipping || !clipping->hasTextAnchor || clipping->textSourceStart != jump.textSourceStart ||
      clipping->textSourceEnd != jump.textSourceEnd || clipping->timestamp != jump.timestamp ||
      clipping->textOffset != jump.textOffset || clipping->textLength != jump.textLength ||
      clipping->startPage != jump.startPage || clipping->endPage != jump.endPage ||
      clipping->wordCount != jump.wordCount) {
    return false;
  }
  std::string text;
  return clippingStore.readText(jump.clippingIndex, text) &&
         ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == jump.textCrc32;
}

bool TxtReaderActivity::persistBookReaderSettings() {
  return txt && bookSettingsWritable &&
         PerBookReaderSettingsStore::save(txt->getCachePath(), bookReaderSettings) ==
             PerBookReaderSettingsStore::SaveStatus::SAVED;
}

void TxtReaderActivity::invalidateReaderLayout() {
  pageOffsets.reset();
  pageOffsetCount = 0;
  pageOffsetCapacity = 0;
  currentPageLines.clear();
  currentPageLineOffsets.clear();
  currentPage = 0;
  lastSuccessfullyRenderedPage = -1;
  lastSavedPage = -1;
  totalPages = 1;
  initializationFailed = false;
  initialized.store(false, std::memory_order_release);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  sdFontSystem.ensureLoaded(renderer, false);
}

void TxtReaderActivity::openBookReaderSettings() {
  if (!bookSettingsWritable) {
    pendingBookSettingsSaveError = true;
    requestUpdate();
    return;
  }
  startActivityForResult(
      std::make_unique<BookReaderSettingsActivity>(renderer, mappedInput, globalReaderSettings, bookReaderSettings,
                                                   BookReaderSettingsActivity::ReaderKind::PlainText),
      [this](const ActivityResult& result) {
        if (result.isCancelled || !std::holds_alternative<ReaderSettingsResult>(result.data)) {
          requestUpdate();
          return;
        }
        const PerBookReaderSettings updated = std::get<ReaderSettingsResult>(result.data).settings;
        if (updated == bookReaderSettings) {
          requestUpdate();
          return;
        }
        saveProgress();
        const PerBookReaderSettings previous = bookReaderSettings;
        bookReaderSettings = updated;
        applyEffectiveBookReaderSettings(globalReaderSettings, bookReaderSettings);
        if (!persistBookReaderSettings()) {
          bookReaderSettings = previous;
          applyEffectiveBookReaderSettings(globalReaderSettings, previous);
          pendingBookSettingsSaveError = true;
        }
        autoPageTurnSeconds = normalizeAutoPageTurnSeconds(
            bookReaderSettings.hasAutoPageTurnInterval ? bookReaderSettings.autoPageTurnSeconds : 0);
        automaticPageTurnActive = bookReaderSettings.autoPageTurnStartsOnOpen && autoPageTurnSeconds != 0;
        invalidateReaderLayout();
        requestUpdate();
      });
}

void TxtReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) return;
  if (!bookSettingsWritable) {
    pendingBookSettingsSaveError = true;
    requestUpdate();
    return;
  }
  saveProgress();
  const PerBookReaderSettings previous = bookReaderSettings;
  SETTINGS.orientation = orientation;
  bookReaderSettings = captureReaderSettings(true, bookReaderSettings.hasAutoPageTurnInterval,
                                             bookReaderSettings.autoPageTurnSeconds,
                                             bookReaderSettings.autoPageTurnStartsOnOpen);
  if (!persistBookReaderSettings()) {
    bookReaderSettings = previous;
    applyEffectiveBookReaderSettings(globalReaderSettings, previous);
    pendingBookSettingsSaveError = true;
  }
  invalidateReaderLayout();
  requestUpdate();
}

void TxtReaderActivity::updateAutoPageTurnFromMenu(const uint8_t seconds) {
  const uint8_t normalized = normalizeAutoPageTurnSeconds(seconds);
  const PerBookReaderSettings previous = bookReaderSettings;
  bookReaderSettings.hasAutoPageTurnInterval = normalized != 0;
  bookReaderSettings.autoPageTurnSeconds = normalized;
  bookReaderSettings.autoPageTurnStartsOnOpen = normalized != 0;
  if (!persistBookReaderSettings()) {
    bookReaderSettings = previous;
    pendingBookSettingsSaveError = true;
  }
  autoPageTurnSeconds = normalizeAutoPageTurnSeconds(
      bookReaderSettings.hasAutoPageTurnInterval ? bookReaderSettings.autoPageTurnSeconds : 0);
  automaticPageTurnActive = bookReaderSettings.autoPageTurnStartsOnOpen && autoPageTurnSeconds != 0;
  lastPageTurnTime = millis();
  requestUpdate();
}

void TxtReaderActivity::jumpToPercent(const int percent) {
  if (!txt || pageOffsetCount == 0) return;
  const int clamped = std::clamp(percent, 0, 100);
  const uint64_t target = static_cast<uint64_t>(txt->getFileSize()) * static_cast<unsigned>(clamped) / 100U;
  jumpToByteOffset(static_cast<uint32_t>(std::min<uint64_t>(target, std::numeric_limits<uint32_t>::max())));
}

void TxtReaderActivity::jumpToByteOffset(const uint32_t byteOffset) {
  if (pageOffsetCount == 0) return;
  const uint32_t* begin = pageOffsets.get();
  const uint32_t* end = begin + pageOffsetCount;
  const uint32_t* after = std::upper_bound(begin, end, byteOffset);
  currentPage = after == begin ? 0 : static_cast<int>(after - begin - 1);
  completionAttemptBlocked = false;
  requestUpdate();
}

void TxtReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  bookmarksWritable = false;
  if (!txt) return;
  const std::string canonical = BookmarkUtil::getBookmarkPath(txt->getPath());
  const std::string legacy = BookmarkUtil::getLegacyBookmarkPath(txt->getPath());
  const std::string path = Storage.exists(canonical.c_str()) ? canonical : legacy;
  const JsonSettingsIO::BookmarkLoadStatus status =
      JsonSettingsIO::loadBookmarksFromFile(cachedBookmarks, path.c_str());
  bookmarksWritable = status == JsonSettingsIO::BookmarkLoadStatus::Loaded ||
                      status == JsonSettingsIO::BookmarkLoadStatus::Missing;
  if (bookmarksWritable) {
    bookmarksWritable = std::all_of(cachedBookmarks.begin(), cachedBookmarks.end(), [](const BookmarkEntry& entry) {
      return entry.positionKind == BookmarkEntry::PositionKind::Text;
    });
  }
  if (!bookmarksWritable) {
    cachedBookmarks.clear();
    pendingBookmarkStorageError = true;
  }
}

void TxtReaderActivity::updateCurrentPageBookmarked() {
  currentPageBookmarked = false;
  if (currentPage < 0 || static_cast<size_t>(currentPage) >= pageOffsetCount || !txt) return;
  const uint32_t start = pageOffsets[currentPage];
  const uint64_t end = static_cast<size_t>(currentPage + 1) < pageOffsetCount ? pageOffsets[currentPage + 1]
                                                                             : txt->getFileSize();
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [start, end](const auto& entry) {
    return entry.positionKind == BookmarkEntry::PositionKind::Text && entry.byteOffset >= start &&
           static_cast<uint64_t>(entry.byteOffset) < end;
  });
}

bool TxtReaderActivity::toggleBookmark() {
  if (!txt || !bookmarksWritable || currentPage < 0 || static_cast<size_t>(currentPage) >= pageOffsetCount) {
    pendingBookmarkStorageError = true;
    return false;
  }
  const uint32_t start = pageOffsets[currentPage];
  const uint64_t end = static_cast<size_t>(currentPage + 1) < pageOffsetCount ? pageOffsets[currentPage + 1]
                                                                             : txt->getFileSize();
  const std::vector<BookmarkEntry> previous = cachedBookmarks;
  const auto first = std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(), [start, end](const auto& entry) {
    return entry.positionKind == BookmarkEntry::PositionKind::Text && entry.byteOffset >= start &&
           static_cast<uint64_t>(entry.byteOffset) < end;
  });
  bookmarkRemoved = first != cachedBookmarks.end();
  cachedBookmarks.erase(first, cachedBookmarks.end());
  if (!bookmarkRemoved) {
    BookmarkEntry bookmark;
    bookmark.positionKind = BookmarkEntry::PositionKind::Text;
    bookmark.byteOffset = start;
    bookmark.percentage = txt->getFileSize() > 0 ? static_cast<float>(start) / txt->getFileSize() : 0.0f;
    std::string summary;
    for (const auto& line : currentPageLines) {
      if (!summary.empty() && !line.empty()) summary.push_back(' ');
      summary += line;
      if (summary.size() > 96) break;
    }
    bookmark.summary = BookmarkUtil::sanitizeBookmarkSummary(std::move(summary));
    if (bookmark.summary.empty()) bookmark.summary = txt->getTitle();
    cachedBookmarks.push_back(std::move(bookmark));
  }
  Storage.mkdir(BookmarkUtil::getBookmarksDir().c_str());
  if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, BookmarkUtil::getBookmarkPath(txt->getPath()).c_str())) {
    cachedBookmarks = previous;
    pendingBookmarkStorageError = true;
    return false;
  }
  updateCurrentPageBookmarked();
  return true;
}

void TxtReaderActivity::openReadingStats() {
  if (!txt) return;

  BookReadingStats displayBookStats;
  GlobalReadingStats displayDeviceStats;
  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  {
    // render() owns currentPage/pageOffsets while painting. Snapshot only
    // after it has finished so the statistics page cannot race a page load.
    RenderLock lock(*this);
    consumeReadingViewSignal();
    stopReadingPage(false, static_cast<uint32_t>(millis()));
    displayBookStats = bookReadingStats;
    displayDeviceStats = globalReadingStats;
    if (!readingSessionCommitted) {
      previewReadingStatsSession(bookReadingStatsWritable ? &displayBookStats : nullptr,
                                 globalReadingStatsWritable ? &displayDeviceStats : nullptr, sessionReadingSeconds,
                                 pendingBookReadingSpans, pendingGlobalReadingSpans,
                                 hasSessionStartLocalDateTime ? &sessionStartLocalDateTime : nullptr);
    }

    if (bookReadingStatsTrusted && bookReadingStats.isCompleted) {
      progress = ReadingStatsMetric::known(100);
    } else if (lastSuccessfullyRenderedPage >= 0 && pageOffsetCount > 0 && txt->getFileSize() > 0) {
      // Report only the page that actually reached the panel. Input can update
      // currentPage before the asynchronous render task has painted it.
      const size_t page = std::min(static_cast<size_t>(lastSuccessfullyRenderedPage), pageOffsetCount - 1);
      const size_t completedOffset = page + 1 < pageOffsetCount ? pageOffsets[page + 1] : txt->getFileSize();
      const uint64_t percent = std::min<uint64_t>(
          100, static_cast<uint64_t>(completedOffset) * 100 / static_cast<uint64_t>(txt->getFileSize()));
      progress = ReadingStatsMetric::estimated(static_cast<uint32_t>(percent));
    }
  }

  const GlobalReadingStatsAggregation allSyncedStats = GlobalReadingStats::loadAggregatedWithReport(displayDeviceStats);
  ReadingStatsDateTime now;
  const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  ReadingStatsPresentation presentation =
      buildReadingStatsPresentation(displayBookStats, bookReadingStatsTrusted, displayDeviceStats,
                                    globalReadingStatsTrusted, allSyncedStats, currentDateTime, progress, false);
  markReadingStatsPageMetricsNotApplicable(presentation);
  startActivityForResult(
      std::make_unique<ReadingStatsActivity>(renderer, mappedInput, txt->getTitle(), std::move(presentation),
                                             ReadingStatsActivity::Page::Book, bookReadingStatsWritable, false),
      [this](const ActivityResult& result) {
        const auto* action = std::get_if<ReadingStatsActionResult>(&result.data);
        if (!action || action->action != ReadingStatsActionResult::Action::EditBookDates || !txt ||
            !bookReadingStatsWritable) {
          requestUpdate();
          return;
        }
        const std::string cachePath = txt->getCachePath();
        startActivityForResult(
            std::make_unique<ReadingStatsDateEditActivity>(renderer, mappedInput, cachePath, bookReadingStats),
            [this, cachePath](const ActivityResult& editResult) {
              if (!editResult.isCancelled) {
                BookReadingStats::LoadStatus status = BookReadingStats::LoadStatus::Invalid;
                bookReadingStats = BookReadingStats::load(cachePath, &status);
                bookReadingStatsTrusted = BookReadingStats::isTrustedLoadStatus(status);
                bookReadingStatsWritable = bookReadingStatsTrusted && BookReadingStats::canPublish(cachePath);
                bookReadingStatsDirty = false;
              }
              requestUpdate();
            });
      });
}

bool TxtReaderActivity::saveProgress() const {
  if (!txt || currentPage < 0 || static_cast<size_t>(currentPage) >= pageOffsetCount) return false;

  const uint32_t byteOffset = static_cast<uint32_t>(pageOffsets[currentPage]);
  uint8_t data[ProgressFileCodec::TXT_V2_SIZE];
  ProgressFileCodec::encodeTxtOffset(byteOffset, data);
  const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(txt->getFileSize()),
                                       totalPages > 0 ? static_cast<uint32_t>(totalPages) : 0};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  if (!ProgressFile::writeTxtAtomic(txt->getCachePath(), data, validator)) {
    LOG_ERR("TRS", "Failed to save progress: page %d, offset %u", currentPage, static_cast<unsigned>(byteOffset));
    return false;
  }
  return true;
}

void TxtReaderActivity::loadProgress() {
  uint8_t data[ProgressFileCodec::TXT_V2_SIZE]{};
  const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(txt->getFileSize()),
                                       totalPages > 0 ? static_cast<uint32_t>(totalPages) : 0};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  const ProgressFile::LoadResult progress = ProgressFile::loadTxt(txt->getCachePath(), data, sizeof(data), validator);
  if (progress) {
    uint32_t savedValue = 0;
    const ProgressFileCodec::TxtDecodeStatus decoded = ProgressFileCodec::decodeTxt(data, progress.size, savedValue);
    if (decoded == ProgressFileCodec::TxtDecodeStatus::Ok) {
      const uint32_t* const offsetsBegin = pageOffsets.get();
      const uint32_t* const offsetsEnd = offsetsBegin + pageOffsetCount;
      const uint32_t* const afterSavedOffset = std::upper_bound(offsetsBegin, offsetsEnd, savedValue);
      currentPage = afterSavedOffset == offsetsBegin ? 0 : static_cast<int>(afterSavedOffset - offsetsBegin - 1);
    } else if (decoded == ProgressFileCodec::TxtDecodeStatus::LegacyPage) {
      currentPage = static_cast<int>(savedValue);
    } else {
      LOG_ERR("TRS", "Loaded TXT progress failed format validation");
      return;
    }
    lastSavedPage = currentPage;
    // Republish a legacy page record as a byte anchor after the first
    // successful render. V2 can remain untouched until a real page turn.
    if (decoded == ProgressFileCodec::TxtDecodeStatus::LegacyPage) lastSavedPage = -1;
    LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
  } else if (progress.source == ProgressFile::LoadSource::Invalid) {
    // A legacy page number can fall beyond the rebuilt page count after a
    // layout change. Clamp only that known old format; malformed/newer V2
    // records remain untouched and are never guessed or overwritten.
    const ProgressFile::LoadResult formatValid = ProgressFile::loadTxt(txt->getCachePath(), data, sizeof(data));
    uint32_t legacyPage = 0;
    if (formatValid && ProgressFileCodec::decodeTxt(data, formatValid.size, legacyPage) ==
                           ProgressFileCodec::TxtDecodeStatus::LegacyPage) {
      currentPage = std::max(totalPages - 1, 0);
      lastSavedPage = -1;
      LOG_DBG("TRS", "Clamped stale progress after repagination: page %d/%d", currentPage, totalPages);
    } else {
      LOG_ERR("TRS", "No valid progress copy could be read");
    }
  } else if (progress.source == ProgressFile::LoadSource::IoError) {
    LOG_ERR("TRS", "No valid progress copy could be read");
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
  // - int32_t: effective line advance (to invalidate cache on line-spacing change)
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

  int32_t lineAdvance = 0;
  serialization::readPod(in, lineAdvance);
  if (lineAdvance != cachedLineAdvance) {
    LOG_DBG("TRS", "Cache line advance mismatch (%d != %d), rebuilding", lineAdvance, cachedLineAdvance);
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
  if (expectedCacheSize != cacheSize || numPages == 0 || numPages > maxPossiblePages || numPages > MAX_INDEX_PAGES) {
    LOG_DBG("TRS", "Page index cache has invalid size or page count, rebuilding");
    return false;
  }

  // Read into staged no-throw storage. A corrupt cache or fragmented heap must
  // not publish a partial table or terminate firmware built without exceptions.
  auto loadedOffsets = makeUniqueNoThrow<uint32_t[]>(numPages);
  if (!loadedOffsets) {
    LOG_ERR("TRS", "Could not allocate %u cached TXT page offsets", static_cast<unsigned>(numPages));
    return false;
  }

  uint32_t previousOffset = 0;
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t pageOffset = 0;
    serialization::readPod(in, pageOffset);
    const bool invalidFirstOffset = i == 0 && pageOffset != 0;
    const bool invalidLaterOffset = i > 0 && pageOffset <= previousOffset;
    const bool pastContent = fileSize == 0 ? pageOffset != 0 : pageOffset >= fileSize;
    if (invalidFirstOffset || invalidLaterOffset || pastContent) {
      LOG_DBG("TRS", "Page index cache has invalid offsets, rebuilding");
      return false;
    }
    loadedOffsets[i] = pageOffset;
    previousOffset = pageOffset;
  }

  pageOffsets = std::move(loadedOffsets);
  pageOffsetCount = numPages;
  pageOffsetCapacity = numPages;
  totalPages = static_cast<int>(pageOffsetCount);
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
  serialization::writePod(out, static_cast<int32_t>(cachedLineAdvance));
  serialization::writePod(out, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(out, cachedParagraphAlignment);
  serialization::writePod(out, static_cast<uint32_t>(pageOffsetCount));

  // Write page offsets
  for (size_t index = 0; index < pageOffsetCount; ++index) {
    serialization::writePod(out, pageOffsets[index]);
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
