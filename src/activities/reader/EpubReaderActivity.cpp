#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>

#include "BookReaderSettingsActivity.h"
#include "BookmarkEntry.h"
#include "ClipSelectionActivity.h"
#include "ClippingListActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "NearbyPositionSyncActivity.h"
#include "PerBookReaderSettingsBridge.h"
#include "PerBookReaderSettingsStore.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "clippings/ClippingPageTools.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

uint8_t pageTurnOptionForRate(const uint8_t rate) {
  if (rate == 0) return 0;
  for (uint8_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) {
    if (PAGE_TURN_RATES[i] == rate) return i;
  }
  return 0;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and all path-keyed user state into /read/.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (moveBookFilePreservingUserState(srcPath, dstPath) != BookPathMoveResult::Moved) {
    LOG_ERR("ERS", "Finished book and its user state could not be moved safely");
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  ImageBlock::clearSessionRenderFailures();

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  const ClippingStore::LoadResult clippingLoad =
      clippingStore.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor());
  if (!clippingStore.isLoaded()) {
    LOG_ERR("ERS", "Clipping store unavailable (status %u)", static_cast<unsigned>(clippingLoad));
  }

  BookReadingStats::LoadStatus bookStatsStatus = BookReadingStats::LoadStatus::Missing;
  bookReadingStats = BookReadingStats::load(epub->getCachePath(), &bookStatsStatus);
  bookReadingStatsWritable = BookReadingStats::isTrustedLoadStatus(bookStatsStatus);
  GlobalReadingStats::LoadStatus globalStatsStatus = GlobalReadingStats::LoadStatus::Missing;
  globalReadingStats = GlobalReadingStats::load(&globalStatsStatus);
  globalReadingStatsWritable = GlobalReadingStats::isTrustedLoadStatus(globalStatsStatus);
  readingSessionTracker = ReadingSessionTracker{};
  sessionReadingSeconds = 0;
  pendingBookReadingSpans = {};
  pendingGlobalReadingSpans = {};
  hasActiveReadingSpanStartLocalDateTime = false;
  hasSessionStartLocalDateTime = false;
  readingSessionCommitted = false;
  bookReadingStatsDirty = false;
  globalReadingStatsDirty = false;
  pendingReadingViewSignal.store(0, std::memory_order_relaxed);

  if (bookReaderSettings.hasAutoPageTurnRate) {
    toggleAutoPageTurn(pageTurnOptionForRate(bookReaderSettings.autoPageTurnRate), false);
  }

  uint8_t data[6]{};
  const int spineCount = epub->getSpineItemsCount();
  const ProgressFile::EpubBounds progressBounds{spineCount > 0 ? static_cast<uint32_t>(spineCount) : 0};
  const ProgressFile::CandidateValidator progressValidator{ProgressFile::validateEpubBounds, &progressBounds};
  const ProgressFile::LoadResult progress =
      ProgressFile::loadEpub(epub->getCachePath(), data, sizeof(data), progressValidator);
  if (progress) {
    const size_t dataSize = progress.size;
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  } else if (progress.source == ProgressFile::LoadSource::Invalid ||
             progress.source == ProgressFile::LoadSource::IoError) {
    LOG_ERR("ERS", "No valid progress copy could be read");
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  commitReadingSession();
  saveReadingStats();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // The per-book overlay must never leak into settings.json or the next book.
  applyReaderSettings(globalReaderSettings);
  // The global profile is active again, so a genuinely missing global SD font
  // may now be repaired in settings.json without persisting any book overlay.
  sdFontSystem.ensureLoaded(renderer);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    clippingStore.unload();
    moveFinishedBookToReadFolder(srcPath, dstPath);
  } else {
    epub.reset();
  }
  clippingStore.unload();
}

void EpubReaderActivity::onPause() {
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
}

void EpubReaderActivity::onResume() {
  // A child leaves its own pixels on the panel until the reader redraws. Only
  // that successful redraw is allowed to restart active reading time.
  pendingReadingViewSignal.store(0, std::memory_order_release);
}

void EpubReaderActivity::signalReadingPageVisible() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(1, std::memory_order_release);
}

void EpubReaderActivity::signalReadingPageHidden() {
  pendingReadingViewAtMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  pendingReadingViewSignal.store(-1, std::memory_order_release);
}

void EpubReaderActivity::consumeReadingViewSignal() {
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

void EpubReaderActivity::recordReadingSample(const ReadingSessionSample& sample) {
  if (sample.seconds > 0) {
    sessionReadingSeconds = addReadingStatsSaturated(sessionReadingSeconds, sample.seconds);
  }
  if (!sample.forwardPageRead) return;

  if (bookReadingStatsWritable) {
    bookReadingStats.totalPagesTurned = addReadingStatsSaturated(bookReadingStats.totalPagesTurned, 1);
    bookReadingStats.recordForwardPageRead(sample.seconds);
    bookReadingStatsDirty = true;
  }
  if (globalReadingStatsWritable) {
    globalReadingStats.totalPagesTurned = addReadingStatsSaturated(globalReadingStats.totalPagesTurned, 1);
    globalReadingStatsDirty = true;
  }
}

void EpubReaderActivity::stopReadingPage(const bool forwardPageTurn, const uint32_t nowMs) {
  const ReadingSessionSample sample = readingSessionTracker.stop(nowMs, forwardPageTurn);
  if (sample.seconds > 0 && hasActiveReadingSpanStartLocalDateTime) {
    pendingBookReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
    pendingGlobalReadingSpans.recordReadingSpan(activeReadingSpanStartLocalDateTime, sample.seconds);
  }
  hasActiveReadingSpanStartLocalDateTime = false;
  recordReadingSample(sample);
}

void EpubReaderActivity::refreshEstimatedTimeLeft() {
  if (!bookReadingStatsWritable) return;
  if (bookReadingStats.isCompleted) {
    if (bookReadingStats.estimatedTimeLeftSeconds != 0) {
      bookReadingStats.estimatedTimeLeftSeconds = 0;
      bookReadingStatsDirty = true;
    }
    return;
  }

  // A partial/in-progress section only knows its current watermark, not its
  // final page count. Keep the previous estimate until pagination is stable.
  if (!epub || !section || section->isBuilding() || section->isPartial() || section->pageCount <= 0 ||
      bookReadingStats.paceSampleCount < 3 || bookReadingStats.avgSecondsPerForwardPage == 0 ||
      epub->getBookSize() == 0) {
    return;
  }

  const size_t previousChapterBytes =
      currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t cumulativeChapterBytes = epub->getCumulativeSpineItemSize(currentSpineIndex);
  if (cumulativeChapterBytes <= previousChapterBytes) return;

  const double chapterBytes = static_cast<double>(cumulativeChapterBytes - previousChapterBytes);
  const double bytesPerPage = chapterBytes / static_cast<double>(section->pageCount);
  const double completedBytes =
      static_cast<double>(previousChapterBytes) + static_cast<double>(section->currentPage) * bytesPerPage;
  if (bytesPerPage <= 0.0 || completedBytes >= static_cast<double>(epub->getBookSize())) return;

  const double remainingPages = (static_cast<double>(epub->getBookSize()) - completedBytes) / bytesPerPage;
  const double estimate = remainingPages * static_cast<double>(bookReadingStats.avgSecondsPerForwardPage);
  const uint32_t estimatedSeconds = estimate >= static_cast<double>(std::numeric_limits<uint32_t>::max())
                                        ? std::numeric_limits<uint32_t>::max()
                                        : static_cast<uint32_t>(estimate + 0.5);
  if (estimatedSeconds > 0 && estimatedSeconds != bookReadingStats.estimatedTimeLeftSeconds) {
    bookReadingStats.estimatedTimeLeftSeconds = estimatedSeconds;
    bookReadingStatsDirty = true;
  }
}

void EpubReaderActivity::commitReadingSession() {
  if (readingSessionCommitted) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  readingSessionCommitted = true;

  // Match CrossInk's noise filters: very short opens do not become reading
  // sessions, while a session must contain at least ten active seconds before
  // its duration is persisted.
  if (sessionReadingSeconds >= 60) {
    if (bookReadingStatsWritable) {
      if (bookReadingStats.sessionCount < std::numeric_limits<uint16_t>::max()) {
        ++bookReadingStats.sessionCount;
      }
      bookReadingStatsDirty = true;
    }
    if (globalReadingStatsWritable) {
      globalReadingStats.totalSessions = addReadingStatsSaturated(globalReadingStats.totalSessions, 1);
      globalReadingStatsDirty = true;
    }
  }

  if (sessionReadingSeconds >= 10) {
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
  refreshEstimatedTimeLeft();
}

void EpubReaderActivity::saveReadingStats() {
  if (bookReadingStatsWritable && bookReadingStatsDirty && epub) {
    if (bookReadingStats.save(epub->getCachePath())) {
      bookReadingStatsDirty = false;
    } else {
      LOG_ERR("ERS", "Failed to save book reading statistics");
    }
  }
  if (globalReadingStatsWritable && globalReadingStatsDirty) {
    if (globalReadingStats.save()) {
      globalReadingStatsDirty = false;
    } else {
      LOG_ERR("ERS", "Failed to save global reading statistics");
    }
  }
}

void EpubReaderActivity::markBookCompleted() {
  if (!bookReadingStatsWritable || bookReadingStats.isCompleted) return;

  bookReadingStats.isCompleted = true;
  bookReadingStats.estimatedTimeLeftSeconds = 0;
  if (!bookReadingStats.finishedDateManual && !bookReadingStats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) bookReadingStats.finishedDate = now.date;
  }
  bookReadingStatsDirty = true;
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          pageTurnOptionForRate(autoPageTurnRate), !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
      [this](const ActivityResult& result) {
        const auto* menu = std::get_if<MenuResult>(&result.data);
        if (!menu) {
          LOG_ERR("ERS", "Reader menu returned an unexpected result type");
          requestUpdate();
          return;
        }
        // Always apply orientation change even if the menu was cancelled
        applyOrientation(menu->orientation);
        toggleAutoPageTurn(menu->pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu->action));
        }
      });
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  // Word geometry must match render(): viewable-area margins plus screen margin.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::loop() {
  consumeReadingViewSignal();
  if (readingSessionTracker.discardIfIdle(static_cast<uint32_t>(millis()))) {
    hasActiveReadingSpanStartLocalDateTime = false;
    LOG_DBG("ERS", "Reading interval discarded after idle threshold");
  }

  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, buildViewportWidth,
                             buildViewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                             SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD)) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. cppcheck can't see the cross-task
    // mutation, so it flags this as always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        stopReadingPage(false, static_cast<uint32_t>(millis()));
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete()) {
        // Finalization can happen entirely in the background while the visible
        // page stays unchanged. Persist the now-exact total here; otherwise an
        // immediate Home action can leave progress.bin carrying the earlier
        // estimate and Dashboard correctly refuses to display it.
        const bool repositioned = applyDeferredReposition();
        const int exactPageCount = section->pageCount;
        if (saveProgress(currentSpineIndex, section->currentPage, exactPageCount)) {
          lastSavedSpineIndex = currentSpineIndex;
          lastSavedPage = section->currentPage;
          lastSavedPageCount = exactPageCount;
        } else {
          pendingSyncSaveError = true;
        }
        if (repositioned || pendingSyncSaveError) requestUpdate();
      }
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  if (atEndOfBook) markBookCompleted();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        // Hold ~0.4s starts dictionary word selection on the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showDictionaryMessage) {
          ignoreNextConfirmRelease = true;  // Prevent menu open on the release that follows
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    consumeReadingViewSignal();
    stopReadingPage(false, static_cast<uint32_t>(millis()));
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_SETTINGS: {
      openBookReaderSettings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      openReadingStats();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING: {
      openClippingSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      openClippings();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          const int backupSpine = currentSpineIndex;
          const int backupPage = section->currentPage;
          // A partial section's pageCount is only its built watermark. Persist
          // the best total estimate before deleting the section cache so a
          // rebuild can restore the same relative position.
          const int backupPageCount = section->estimatedTotalPages();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
            pendingSyncSaveError = true;
            requestUpdate();
            break;
          }
          nextPageNumber = backupPage;
          cachedChapterTotalPageCount = backupPageCount;
          section.reset();
          if (!clearBookCacheDirectoryPreservingUserState(epub->getCachePath())) {
            LOG_ERR("ERS", "Failed to clear derived book cache without risking user data");
            // A failed rollback may leave the only authoritative copy of some
            // per-book files in the sibling staging directory. Recover it now;
            // if that is still impossible, release the book and leave before
            // render()/onExit() can create conflicting state in the cache.
            if (!recoverBookCacheUserState(epub->getCachePath(), epub->getPath())) {
              LOG_ERR("ERS", "Cache state recovery remains incomplete; leaving reader fail-closed");
              bookSettingsWritable = false;
              bookReadingStatsWritable = false;
              epub.reset();
              lock.unlock();
              onGoHome();
              return;
            }
            pendingCacheClearError = true;
            requestUpdate();
            break;
          }
          epub->setupCacheDir();
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC: {
      launchNearbyPositionSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch
  if (pendingReadFolderMove) {
    pendingFinishedMoveSyncError = true;
    requestUpdate();
    return true;
  }

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // The reader releases its Epub object before ActivityManager can run
  // onExit(), so persist the finished session while the cache path still exists.
  commitReadingSession();
  saveReadingStats();

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

bool EpubReaderActivity::launchNearbyPositionSync() {
  if (!epub) return false;
  if (pendingReadFolderMove) {
    pendingFinishedMoveSyncError = true;
    requestUpdate();
    return true;
  }

  const CrossPointPosition localPosition = getCurrentPosition();
  const SavedProgressPosition savedPosition = ProgressMapper::toSavedProgress(epub, localPosition);
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  const std::string chapterName = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : "";
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  // Nearby leaves through a radio-cleanup restart. Replace the reader (instead
  // of stacking a child) so onExit commits reading statistics and restores the
  // per-book settings overlay before any possible reboot.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }
  commitReadingSession();
  saveReadingStats();
  const NearbyReaderLayout nearbyLayout{
      SETTINGS.getReaderFontId(),
      SETTINGS.getReaderLineCompression(),
      SETTINGS.extraParagraphSpacing != 0,
      SETTINGS.paragraphAlignment,
      buildViewportWidth,
      buildViewportHeight,
      SETTINGS.hyphenationEnabled != 0,
      SETTINGS.embeddedStyle != 0,
      SETTINGS.imageRendering,
      SETTINGS.focusReadingEnabled != 0,
  };
  activityManager.replaceActivity(std::make_unique<NearbyPositionSyncActivity>(
      renderer, mappedInput, epub, localPosition, savedPosition, chapterName, nearbyLayout));
  return true;
}

bool EpubReaderActivity::persistBookReaderSettings() {
  if (!epub || !bookSettingsWritable) return false;

  bookReaderSettings.hasAutoPageTurnRate = autoPageTurnRate > 0;
  bookReaderSettings.autoPageTurnRate = autoPageTurnRate;

  PerBookReaderSettings normalizedBook = bookReaderSettings;
  PerBookReaderSettings normalizedGlobal = globalReaderSettings;
  normalizedBook.hasReaderOverrides = false;
  normalizedBook.hasAutoPageTurnRate = false;
  normalizedBook.autoPageTurnRate = 0;
  normalizedGlobal.hasReaderOverrides = false;
  normalizedGlobal.hasAutoPageTurnRate = false;
  normalizedGlobal.autoPageTurnRate = 0;
  if (!bookReaderSettings.hasReaderOverrides && !bookReaderSettings.hasAutoPageTurnRate &&
      normalizedBook == normalizedGlobal) {
    return PerBookReaderSettingsStore::clear(epub->getCachePath());
  }
  return PerBookReaderSettingsStore::save(epub->getCachePath(), bookReaderSettings) ==
         PerBookReaderSettingsStore::SaveStatus::SAVED;
}

void EpubReaderActivity::invalidateReaderLayout() {
  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  sdFontSystem.ensureLoaded(renderer, false);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  section.reset();
}

void EpubReaderActivity::openBookReaderSettings() {
  if (!bookSettingsWritable) {
    pendingBookSettingsSaveError = true;
    requestUpdate();
    return;
  }

  startActivityForResult(
      std::make_unique<BookReaderSettingsActivity>(renderer, mappedInput, globalReaderSettings, bookReaderSettings),
      [this](const ActivityResult& result) {
        if (result.isCancelled || !std::holds_alternative<ReaderSettingsResult>(result.data)) return;
        PerBookReaderSettings updated = std::get<ReaderSettingsResult>(result.data).settings;
        if (updated == bookReaderSettings) return;

        bookReaderSettings = updated;
        toggleAutoPageTurn(
            pageTurnOptionForRate(bookReaderSettings.hasAutoPageTurnRate ? bookReaderSettings.autoPageTurnRate : 0),
            false);
        if (!persistBookReaderSettings()) pendingBookSettingsSaveError = true;
        invalidateReaderLayout();
        requestUpdate();
      });
}

void EpubReaderActivity::openReadingStats() {
  if (!epub) return;

  // Opening the statistics screen is a real visibility boundary. Consume the
  // current page interval now so the numbers on the screen include it; the
  // redraw after returning starts a fresh interval.
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  refreshEstimatedTimeLeft();
  BookReadingStats displayBookStats = bookReadingStats;
  if (bookReadingStatsWritable) {
    displayBookStats.totalReadingSeconds =
        addReadingStatsSaturated(displayBookStats.totalReadingSeconds, sessionReadingSeconds);
    for (size_t i = 0; i < displayBookStats.timeOfDaySeconds.size(); ++i) {
      displayBookStats.timeOfDaySeconds[i] =
          addReadingStatsSaturated(displayBookStats.timeOfDaySeconds[i], pendingBookReadingSpans.timeOfDaySeconds[i]);
    }
    for (size_t i = 0; i < displayBookStats.dayOfWeekSeconds.size(); ++i) {
      displayBookStats.dayOfWeekSeconds[i] =
          addReadingStatsSaturated(displayBookStats.dayOfWeekSeconds[i], pendingBookReadingSpans.dayOfWeekSeconds[i]);
    }
  }

  GlobalReadingStats displayGlobalStats = GlobalReadingStats::hasSyncedStats()
                                              ? GlobalReadingStats::loadAggregated(globalReadingStats)
                                              : globalReadingStats;
  if (globalReadingStatsWritable) {
    displayGlobalStats.totalReadingSeconds =
        addReadingStatsSaturated(displayGlobalStats.totalReadingSeconds, sessionReadingSeconds);
    displayGlobalStats.merge(pendingGlobalReadingSpans);
  }

  startActivityForResult(std::make_unique<ReadingStatsActivity>(renderer, mappedInput, epub->getTitle(),
                                                                displayBookStats, displayGlobalStats),
                         [](const ActivityResult&) {});
}

void EpubReaderActivity::openClippingSelection() {
  if (!epub || !section || !clippingStore.isLoaded()) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }
  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    pendingClippingNotice = ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) {
    pendingClippingNotice = ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
  const uint16_t pageCount = std::max<uint16_t>(section->estimatedTotalPages(), section->pageCount);
  const uint16_t paragraphIndex = section->getParagraphIndexForPage(currentPage).value_or(UINT16_MAX);

  startActivityForResult(
      std::make_unique<ClipSelectionActivity>(renderer, mappedInput, std::move(page), SETTINGS.getReaderFontId(),
                                              marginLeft, marginTop, currentPage, pageCount, paragraphIndex),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* selection = std::get_if<ClippingSelectionResult>(&result.data);
        if (!selection || !epub || !clippingStore.isLoaded()) {
          pendingClippingNotice = ClippingNotice::Unavailable;
          requestUpdate();
          return;
        }

        ClippingCodec::ClippingMetadata clipping;
        clipping.spineIndex = static_cast<uint16_t>(currentSpineIndex);
        clipping.startPage = selection->startPage;
        clipping.endPage = selection->endPage;
        clipping.pageCount = selection->pageCount;
        // These are stable indexes among all visible tokens on the page. The
        // UI-vector indexes are intentionally not persisted.
        clipping.startWordIndex = selection->startPageWordIndex;
        clipping.endWordIndex = selection->endPageWordIndex;
        clipping.wordCount = selection->wordCount;
        clipping.paragraphIndex = selection->paragraphIndex;
        clipping.pageFingerprint = selection->pageFingerprint;
        const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
        if (tocIndex >= 0) clipping.chapterTitle = epub->getTocItem(tocIndex).title;
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

void EpubReaderActivity::openClippings() {
  if (!epub || !clippingStore.isLoaded()) {
    pendingClippingNotice = clippingStore.lastCodecStatus() == ClippingCodec::Status::NewerVersion
                                ? ClippingNotice::NewerFormat
                                : ClippingNotice::Unavailable;
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<ClippingListActivity>(renderer, mappedInput, clippingStore),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* jump = std::get_if<ClippingJumpResult>(&result.data);
                           if (!jump || !epub || jump->spineIndex >= epub->getSpineItemsCount()) {
                             pendingClippingNotice = ClippingNotice::JumpUnavailable;
                             requestUpdate();
                             return;
                           }

                           pendingClippingJump = PendingClippingJump{jump->spineIndex, jump->startPage, jump->pageCount,
                                                                     jump->paragraphIndex, jump->pageFingerprint};
                           if (currentSpineIndex != jump->spineIndex) {
                             RenderLock lock(*this);
                             currentSpineIndex = jump->spineIndex;
                             nextPageNumber = 0;
                             section.reset();
                           }
                           requestUpdate();
                         });
}

void EpubReaderActivity::applyPendingClippingJump(const int marginLeft, const int marginTop) {
  if (!pendingClippingJump || !section || currentSpineIndex != pendingClippingJump->spineIndex) return;

  const PendingClippingJump jump = *pendingClippingJump;
  bool exactPage = false;
  if (jump.pageFingerprint != 0 && jump.page < section->pageCount) {
    const std::unique_ptr<Page> page = section->loadPage(jump.page);
    exactPage = page && ClippingPageTools::fingerprint(*page, renderer, SETTINGS.getReaderFontId(), marginLeft,
                                                       marginTop) == jump.pageFingerprint;
  }

  if (exactPage) {
    section->currentPage = jump.page;
  } else {
    // A paragraph can span several rendered pages, so its paragraph LUT entry
    // is not an exact page identity. Refuse instead of jumping to the first
    // page of an ambiguous paragraph or scaling an old page number.
    pendingClippingNotice = ClippingNotice::JumpUnavailable;
  }
  pendingClippingJump.reset();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // This singleton is the active per-book overlay. Persist it in the book's
    // own file, never in the global settings.json.
    SETTINGS.orientation = orientation;
    bookReaderSettings = captureReaderSettings(true, autoPageTurnRate > 0, autoPageTurnRate);
    if (!persistBookReaderSettings()) pendingBookSettingsSaveError = true;

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption, const bool persist) {
  const uint8_t previousRate = autoPageTurnRate;
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    autoPageTurnRate = 0;
    if (persist && previousRate != 0 && !persistBookReaderSettings()) pendingBookSettingsSaveError = true;
    return;
  }

  autoPageTurnRate = PAGE_TURN_RATES[selectedPageTurnOption];
  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  if (persist && previousRate != autoPageTurnRate && !persistBookReaderSettings()) {
    pendingBookSettingsSaveError = true;
  }

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  consumeReadingViewSignal();
  stopReadingPage(isForwardTurn, static_cast<uint32_t>(millis()));

  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building/partial (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding() || section->isPartial()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  refreshEstimatedTimeLeft();
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    signalReadingPageHidden();
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (pendingFinishedMoveSyncError) {
      pendingFinishedMoveSyncError = false;
      GUI.drawPopup(renderer, tr(STR_SYNC_AFTER_FINISHED_MOVE));
    } else if (pendingSyncSaveError) {
      pendingSyncSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    } else if (pendingBookSettingsSaveError) {
      pendingBookSettingsSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_BOOK_SETTINGS_FAILED));
    } else if (pendingCacheClearError) {
      pendingCacheClearError = false;
      GUI.drawPopup(renderer, tr(STR_CLEAR_CACHE_FAILED));
    } else if (pendingClippingNotice != ClippingNotice::None) {
      const ClippingNotice notice = pendingClippingNotice;
      pendingClippingNotice = ClippingNotice::None;
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
    }
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    signalReadingPageHidden();
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    signalReadingPageHidden();
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Lend the framebuffer's 48 KB to the blocking pre-render burst
          // (startBuild inflates the whole spine HTML — the memory peak). The
          // background buildSomeMore chunks in loop() do NOT get the loan: they
          // deliberately interleave with page renders. Restored before render.
          GfxRenderer::FrameBufferLoan loan(renderer);
          if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                   SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                   viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                   SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            loan.end();  // restore before anything draws (showBuildError renders a popup)
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              loan.end();  // restore before anything draws (showBuildError renders a popup)
              showBuildError();
              return;
            }
          }
          loan.end();
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() &&
        !section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                             SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                             SETTINGS.focusReadingEnabled)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();
  applyPendingClippingJump(orientedMarginLeft, orientedMarginTop);

  renderer.clearScreen();

  if (section->pageCount == 0) {
    signalReadingPageHidden();
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    signalReadingPageHidden();
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        signalReadingPageHidden();
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    signalReadingPageVisible();
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const uint32_t pageFingerprint =
      clippingStore.isLoaded() && clippingStore.size() > 0
          ? ClippingPageTools::fingerprint(*page, renderer, fontId, orientedMarginLeft, orientedMarginTop)
          : 0;
  const ClippingPageTools::HighlightPlan clippingHighlights =
      pageFingerprint != 0 && section
          ? ClippingPageTools::buildExactHighlightPlan(
                renderer, *page, fontId, orientedMarginLeft, orientedMarginTop, clippingStore.entries(),
                static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage), pageFingerprint)
          : ClippingPageTools::HighlightPlan{};
  const auto drawClippingHighlights = [&]() { clippingHighlights.draw(renderer); };
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    drawClippingHighlights();
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    drawClippingHighlights();
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  drawClippingHighlights();
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      drawClippingHighlights();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  consumeReadingViewSignal();
  stopReadingPage(false, static_cast<uint32_t>(millis()));
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string canonicalPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string legacyPath = BookmarkUtil::getLegacyBookmarkPath(epub->getPath());
  const std::string bmPath = Storage.exists(canonicalPath.c_str()) ? canonicalPath : legacyPath;
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
