#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <memory>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubRenderBenchmarkActivity.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "StarredPagesActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};

#if DEBUG_MEMORY_CONSUMPTION
void logReaderMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}
#else
inline void logReaderMemSnapshot(const char*) {}
#endif

// Computes the [0..100] EPUB progress percent. Returns 0 when pageCount is unknown (sync/bookmark
// pre-render writes), in which case the next saveProgress() will overwrite progress.bin with the
// real value before the user can leave the reader.
uint8_t epubProgressPercentByte(const Epub& epub, const int spineIndex, const int currentPage, const int pageCount) {
  if (pageCount <= 0) {
    return 0;
  }
  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  return ReaderUtils::fractionProgressPercentByte(epub.calculateProgress(spineIndex, chapterProgress));
}

// Writes the canonical EPUB progress.bin layout: spine(2) + page(2) + pageCount(2) + percent(1).
// Used by the per-page saveProgress() and by transient writers (sync restore, bookmark jump) so
// the on-disk format stays consistent regardless of caller.
bool writeReaderProgressCache(const std::string& cachePath, const int spineIndex, const int currentPage,
                              const int pageCount, const uint8_t percent) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + "/progress.bin", f)) {
    LOG_ERR("ERS", "Failed to open progress cache: %s", cachePath.c_str());
    return false;
  }

  uint8_t data[7];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = percent;
  f.write(data, 7);
  f.close();
  return true;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

const char* orientationToString(const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      return "Portrait";
    case GfxRenderer::LandscapeClockwise:
      return "Landscape CW";
    case GfxRenderer::PortraitInverted:
      return "Portrait Inverted";
    case GfxRenderer::LandscapeCounterClockwise:
      return "Landscape CCW";
  }
  return "Unknown";
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  logReaderMemSnapshot("onEnter_begin");

  // Drop any input events that arrived from the activity that launched us (e.g. a wake-up power
  // button hold) before they reach detectPageTurn() — see ReaderUtils::InputDrainGuard.
  inputDrainGuard.arm();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }
  logReaderMemSnapshot("onEnter_after_orientation");

  epub->setupCacheDir();
  logReaderMemSnapshot("onEnter_after_setupCacheDir");
  applyPendingSyncSession();
  applyPendingBookmarkJump();
  logReaderMemSnapshot("onEnter_after_pending_sync");

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
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
  logReaderMemSnapshot("onEnter_after_progress_load");

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());
  logReaderMemSnapshot("onEnter_after_bookmarks_loaded");

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  std::string series = epub->getSeries();
  if (!series.empty() && !epub->getSeriesIndex().empty()) {
    series += " #" + epub->getSeriesIndex();
  }
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), series, epub->getThumbBmpPath());
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(epub->getPath());
  bookEmbeddedStyleOverride = currentBook.embeddedStyleOverride;
  bookImageRenderingOverride = currentBook.imageRenderingOverride;
  bookFontFamilyOverride = currentBook.fontFamilyOverride;
  bookFontSizeOverride = currentBook.fontSizeOverride;
  logReaderMemSnapshot("onEnter_after_recent_books");

  // Trigger first update
  logReaderMemSnapshot("onEnter_before_request_update");
  requestUpdate();
  logReaderMemSnapshot("onEnter_ready");
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  logReaderMemSnapshot("onExit_before_release");

  // Save bookmarks before exit
  bookmarkStore.save();
  if (epub) {
    GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(), false);
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
  currentPageFootnotes.clear();
  currentPageFootnotes.shrink_to_fit();
  logReaderMemSnapshot("onExit_after_release");
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      buttonEvents.drain();
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

  bool delayedPrevTurn = false;
  bool delayedNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm) {
      if (ev.type == ButtonEventManager::PressType::Long && KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::COMPARE);
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        openReaderMenu();
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (footnoteDepth > 0) {
          restoreSavedPosition();
          return;
        }
        ReaderUtils::enforceExitFullRefresh(renderer);
        finish();
        return;
      }
    }

    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT &&
           ButtonEventManager::hasDoubleAction(MappedInputManager::Button::PageBack)) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT &&
           ButtonEventManager::hasDoubleAction(MappedInputManager::Button::Left))) {
        delayedPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT &&
           ButtonEventManager::hasDoubleAction(MappedInputManager::Button::PageForward)) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT &&
           ButtonEventManager::hasDoubleAction(MappedInputManager::Button::Right))) {
        delayedNextTurn = true;
        continue;
      }
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    if (!delayedPrevTurn && !delayedNextTurn) {
      return;
    }
    prevTriggered = delayedPrevTurn;
    nextTriggered = delayedNextTurn;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button returns to caller and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      finish();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
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
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                 : epub->getTocIndexForSpineIndex(currentSpineIndex);
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx, tocIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            RenderLock lock(*this);
            const auto& chapter = std::get<ChapterResult>(result.data);
            auto resolvedPage = (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                    ? section->getPageForTocIndex(*chapter.tocIndex)
                                    : std::nullopt;
            if (resolvedPage) {
              section->currentPage = *resolvedPage;
            } else {
              pendingTocIndex = chapter.tocIndex;
              currentSpineIndex = chapter.spineIndex;
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
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STAR_PAGE: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STARRED_PAGES: {
      startActivityForResult(
          std::make_unique<StarredPagesActivity>(renderer, mappedInput, bookmarkStore, epub),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& starred = std::get<StarredPageResult>(result.data);
              if (currentSpineIndex != starred.spineIndex || !section || section->currentPage != starred.pageNumber) {
                RenderLock lock(*this);
                currentSpineIndex = starred.spineIndex;
                nextPageNumber = starred.pageNumber;
                section.reset();
              }
            }
          });
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
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
          if (!bookmarkStore.isEmpty()) {
            bookmarkStore.markDirty();
            bookmarkStore.save();
            GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                           false);
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::RENDER_BENCHMARK: {
      runRenderBenchmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PULL_REMOTE: {
      // One-tap pull path: run network preconditions and apply remote progress
      // directly instead of showing an intermediate chooser screen.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PULL_REMOTE);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PUSH_LOCAL: {
      // One-tap push path: run network preconditions and upload local progress
      // directly for KOReader-like "sync now" behavior.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PUSH_LOCAL);
      }
      break;
    }
  }
}

void EpubReaderActivity::runRenderBenchmark() {
  if (!epub) {
    return;
  }

  if (!section) {
    requestUpdateAndWait();
    if (!section) {
      return;
    }
  }

  const LastRenderStats startSnapshot = lastRenderStats;
  BenchmarkAggregate aggregate;
  auto recordRender = [&aggregate](const LastRenderStats& snapshot) {
    if (!snapshot.valid) {
      return;
    }

    aggregate.renderCount++;
    aggregate.imagePageCount += snapshot.hadImages ? 1 : 0;
    aggregate.cacheRebuildCount += snapshot.cacheRebuilt ? 1 : 0;
    if (snapshot.footnoteCount > aggregate.maxFootnotes) {
      aggregate.maxFootnotes = snapshot.footnoteCount;
    }

    aggregate.totalRequestRenderMs += snapshot.requestRenderMs;
    if (aggregate.renderCount == 1 || snapshot.requestRenderMs < aggregate.minRequestRenderMs) {
      aggregate.minRequestRenderMs = snapshot.requestRenderMs;
    }
    if (snapshot.requestRenderMs > aggregate.maxRequestRenderMs) {
      aggregate.maxRequestRenderMs = snapshot.requestRenderMs;
    }

    aggregate.totalRenderMs += snapshot.phases.totalMs;
    if (aggregate.renderCount == 1 || snapshot.phases.totalMs < aggregate.minRenderMs) {
      aggregate.minRenderMs = snapshot.phases.totalMs;
    }
    if (snapshot.phases.totalMs > aggregate.maxRenderMs) {
      aggregate.maxRenderMs = snapshot.phases.totalMs;
    }

    aggregate.totalSectionLoadMs += snapshot.sectionLoadMs;
    aggregate.totalPageLoadMs += snapshot.pageLoadMs;
    aggregate.totalPhases.prewarmMs += snapshot.phases.prewarmMs;
    aggregate.totalPhases.bwRenderMs += snapshot.phases.bwRenderMs;
    aggregate.totalPhases.displayMs += snapshot.phases.displayMs;
    aggregate.totalPhases.bwStoreMs += snapshot.phases.bwStoreMs;
    aggregate.totalPhases.grayLsbMs += snapshot.phases.grayLsbMs;
    aggregate.totalPhases.grayMsbMs += snapshot.phases.grayMsbMs;
    aggregate.totalPhases.grayDisplayMs += snapshot.phases.grayDisplayMs;
    aggregate.totalPhases.bwRestoreMs += snapshot.phases.bwRestoreMs;
    aggregate.totalPhases.totalMs += snapshot.phases.totalMs;

    aggregate.totalFontCacheHits += snapshot.fontCacheHits;
    aggregate.totalFontCacheMisses += snapshot.fontCacheMisses;
    aggregate.totalFontDecompressMs += snapshot.fontDecompressMs;
    aggregate.totalFontGetBitmapTimeUs += snapshot.fontGetBitmapTimeUs;
    aggregate.totalFontGetBitmapCalls += snapshot.fontGetBitmapCalls;

    if (aggregate.renderCount == 1 || snapshot.freeHeapAfter < aggregate.minFreeHeapAfter) {
      aggregate.minFreeHeapAfter = snapshot.freeHeapAfter;
    }
    if (snapshot.freeHeapAfter > aggregate.maxFreeHeapAfter) {
      aggregate.maxFreeHeapAfter = snapshot.freeHeapAfter;
    }
  };
  const unsigned long startTime = millis();
  int forwardTurns = 0;
  int backwardTurns = 0;

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(true)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    forwardTurns++;
  }

  const unsigned long forwardMs = millis() - startTime;
  const unsigned long backwardStart = millis();

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(false)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    backwardTurns++;
  }

  const unsigned long backwardMs = millis() - backwardStart;

  startActivityForResult(
      std::make_unique<EpubRenderBenchmarkActivity>(
          renderer, mappedInput,
          buildRenderBenchmarkReport(startSnapshot, aggregate, forwardTurns, forwardMs, backwardTurns, backwardMs)),
      [this](const ActivityResult&) { requestUpdate(); });
}

std::string EpubReaderActivity::buildRenderBenchmarkReport(const LastRenderStats& startSnapshot,
                                                           const BenchmarkAggregate& aggregate, const int forwardTurns,
                                                           const unsigned long forwardMs, const int backwardTurns,
                                                           const unsigned long backwardMs) const {
  const LastRenderStats& endSnapshot = lastRenderStats.valid ? lastRenderStats : startSnapshot;

  std::string report;
  report.reserve(768);

  auto appendLine = [&report](const std::string& line) {
    if (!report.empty()) {
      report += '\n';
    }
    report += line;
  };

  appendLine("Forward 10: " + std::to_string(forwardTurns) + " turns in " + std::to_string(forwardMs) + " ms");
  if (forwardTurns > 0) {
    appendLine("Forward avg: " + std::to_string(forwardMs / static_cast<unsigned long>(forwardTurns)) + " ms/turn");
  }
  appendLine("Backward 10: " + std::to_string(backwardTurns) + " turns in " + std::to_string(backwardMs) + " ms");
  if (backwardTurns > 0) {
    appendLine("Backward avg: " + std::to_string(backwardMs / static_cast<unsigned long>(backwardTurns)) + " ms/turn");
  }
  appendLine("Measured renders: " + std::to_string(aggregate.renderCount) + ", image pages " +
             std::to_string(aggregate.imagePageCount) + ", cache rebuilds " +
             std::to_string(aggregate.cacheRebuildCount));

  appendLine("Start: spine " + std::to_string(startSnapshot.spineIndex) + ", page " +
             std::to_string(startSnapshot.pageIndex + 1) + "/" + std::to_string(startSnapshot.pageCount));
  appendLine("End: spine " + std::to_string(endSnapshot.spineIndex) + ", page " +
             std::to_string(endSnapshot.pageIndex + 1) + "/" + std::to_string(endSnapshot.pageCount));
  appendLine("Orientation: " +
             std::string(orientationToString(static_cast<GfxRenderer::Orientation>(endSnapshot.orientation))));
  appendLine("Viewport: " + std::to_string(endSnapshot.viewportWidth) + "x" +
             std::to_string(endSnapshot.viewportHeight) + " px, margins T/R/B/L " +
             std::to_string(endSnapshot.marginTop) + "/" + std::to_string(endSnapshot.marginRight) + "/" +
             std::to_string(endSnapshot.marginBottom) + "/" + std::to_string(endSnapshot.marginLeft));
  appendLine("Font: " + std::to_string(endSnapshot.effectiveFontId) + ", embedded CSS " +
             std::string(endSnapshot.embeddedStyle ? "on" : "off") + ", images " +
             std::to_string(endSnapshot.imageRendering) + ", AA " +
             std::string(endSnapshot.textAntiAliasing ? "on" : "off"));
  appendLine("Last page: images " + std::string(endSnapshot.hadImages ? "yes" : "no") + ", footnotes " +
             std::to_string(endSnapshot.footnoteCount) + ", cache rebuilt " +
             std::string(endSnapshot.cacheRebuilt ? "yes" : "no"));
  if (aggregate.renderCount > 0) {
    appendLine("Render avg/min/max: request " +
               std::to_string(aggregate.totalRequestRenderMs / static_cast<unsigned long>(aggregate.renderCount)) +
               "/" + std::to_string(aggregate.minRequestRenderMs) + "/" + std::to_string(aggregate.maxRequestRenderMs) +
               " ms, core " +
               std::to_string(aggregate.totalRenderMs / static_cast<unsigned long>(aggregate.renderCount)) + "/" +
               std::to_string(aggregate.minRenderMs) + "/" + std::to_string(aggregate.maxRenderMs) + " ms");
    appendLine("Aggregate loads: section " + std::to_string(aggregate.totalSectionLoadMs) + " ms, page " +
               std::to_string(aggregate.totalPageLoadMs) + " ms, max footnotes " +
               std::to_string(aggregate.maxFootnotes));
    appendLine("Aggregate phases: prewarm " + std::to_string(aggregate.totalPhases.prewarmMs) + ", bw " +
               std::to_string(aggregate.totalPhases.bwRenderMs) + ", display " +
               std::to_string(aggregate.totalPhases.displayMs) + ", gray total " +
               std::to_string(aggregate.totalPhases.grayLsbMs + aggregate.totalPhases.grayMsbMs +
                              aggregate.totalPhases.grayDisplayMs));
    appendLine("Aggregate font: hits " + std::to_string(aggregate.totalFontCacheHits) + ", misses " +
               std::to_string(aggregate.totalFontCacheMisses) + ", decompress " +
               std::to_string(aggregate.totalFontDecompressMs) + " ms");
    appendLine("Aggregate glyph lookups: " + std::to_string(aggregate.totalFontGetBitmapCalls) + " calls, " +
               std::to_string(aggregate.totalFontGetBitmapTimeUs) + " us total");
    appendLine("Heap after render min/max: " + std::to_string(aggregate.minFreeHeapAfter) + "/" +
               std::to_string(aggregate.maxFreeHeapAfter));
  }
  appendLine("Last render: request " + std::to_string(endSnapshot.requestRenderMs) + " ms, section load " +
             std::to_string(endSnapshot.sectionLoadMs) + " ms, page load " + std::to_string(endSnapshot.pageLoadMs) +
             " ms, render total " + std::to_string(endSnapshot.phases.totalMs) + " ms");
  appendLine("Phases: prewarm " + std::to_string(endSnapshot.phases.prewarmMs) + ", bw " +
             std::to_string(endSnapshot.phases.bwRenderMs) + ", display " +
             std::to_string(endSnapshot.phases.displayMs) + ", store " + std::to_string(endSnapshot.phases.bwStoreMs) +
             ", gray lsb " + std::to_string(endSnapshot.phases.grayLsbMs) + ", gray msb " +
             std::to_string(endSnapshot.phases.grayMsbMs) + ", gray display " +
             std::to_string(endSnapshot.phases.grayDisplayMs) + ", restore " +
             std::to_string(endSnapshot.phases.bwRestoreMs));
  appendLine("Font cache: hits " + std::to_string(endSnapshot.fontCacheHits) + ", misses " +
             std::to_string(endSnapshot.fontCacheMisses) + ", decompress " +
             std::to_string(endSnapshot.fontDecompressMs) + " ms, groups " +
             std::to_string(endSnapshot.fontUniqueGroups));
  appendLine("Font buffers: page " + std::to_string(endSnapshot.fontPageBufferBytes) + ", glyph table " +
             std::to_string(endSnapshot.fontPageGlyphsBytes) + ", peak temp " +
             std::to_string(endSnapshot.fontPeakTempBytes));
  appendLine("Glyph lookups: " + std::to_string(endSnapshot.fontGetBitmapCalls) + " calls, " +
             std::to_string(endSnapshot.fontGetBitmapTimeUs) + " us total");
  appendLine("Heap: before " + std::to_string(endSnapshot.freeHeapBefore) + "/" +
             std::to_string(endSnapshot.largestFreeBlockBefore) + ", after " +
             std::to_string(endSnapshot.freeHeapAfter) + "/" + std::to_string(endSnapshot.largestFreeBlockAfter));

  return report;
}

void EpubReaderActivity::launchKOReaderSync(const SyncLaunchMode mode) {
  if (!epub) {
    return;
  }

  const int currentPage = section ? section->currentPage : 0;
  const int totalPages = section ? section->pageCount : 0;
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;
  if (mode == SyncLaunchMode::PULL_REMOTE) {
    syncIntent = KOReaderSyncIntentState::PULL_REMOTE;
  } else if (mode == SyncLaunchMode::PUSH_LOCAL) {
    syncIntent = KOReaderSyncIntentState::PUSH_LOCAL;
  }

  auto& sync = APP_STATE.koReaderSyncSession;
  sync.active = true;
  sync.epubPath = epub->getPath();
  sync.spineIndex = currentSpineIndex;
  sync.page = currentPage;
  sync.totalPagesInSpine = totalPages;
  // Populate paragraph index and XHTML seek hint from section LUT if available.
  if (section) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      sync.paragraphIndex = *pIdx;
      sync.hasParagraphIndex = true;
      if (const auto hint = section->getXhtmlByteOffsetForPage(static_cast<uint16_t>(currentPage))) {
        sync.xhtmlSeekHint = *hint;
      } else {
        sync.xhtmlSeekHint = 0;
      }
    } else {
      sync.paragraphIndex = 0;
      sync.hasParagraphIndex = false;
      sync.xhtmlSeekHint = 0;
    }
  } else {
    sync.paragraphIndex = 0;
    sync.hasParagraphIndex = false;
    sync.xhtmlSeekHint = 0;
  }
  sync.intent = syncIntent;
  sync.outcome = KOReaderSyncOutcomeState::PENDING;
  sync.resultSpineIndex = 0;
  sync.resultPage = 0;
  sync.resultParagraphIndex = 0;
  sync.resultHasParagraphIndex = false;
  APP_STATE.saveToFile();

  LOG_DBG("ERS", "Standalone sync handoff: spine=%d page=%d/%d", currentSpineIndex, currentPage, totalPages);
  logReaderMemSnapshot("before_replace_with_sync");
  activityManager.goToKOReaderSync();
}

void EpubReaderActivity::applyPendingSyncSession() {
  auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || !epub || sync.epubPath != epub->getPath()) {
    return;
  }

  LOG_DBG("ERS", "Applying pending sync session outcome=%d path=%s", static_cast<int>(sync.outcome),
          sync.epubPath.c_str());

  // Upload-complete returns to the same local position the reader already persisted
  // before sync launched, so there is no need to rewrite progress.bin here.
  if (sync.outcome == KOReaderSyncOutcomeState::UPLOAD_COMPLETE) {
    LOG_DBG("ERS", "Upload-complete resume keeps existing local progress.bin unchanged");
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  int restoreSpineIndex = sync.spineIndex;
  int restorePage = sync.page;
  pendingParagraphLookup = sync.hasParagraphIndex;
  pendingParagraphIndex = sync.paragraphIndex;

  if (sync.outcome == KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    restoreSpineIndex = sync.resultSpineIndex;
    restorePage = sync.resultPage;
    pendingParagraphLookup = sync.resultHasParagraphIndex;
    pendingParagraphIndex = sync.resultParagraphIndex;
    LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d paragraph=%u hasParagraph=%s", restoreSpineIndex,
            restorePage, pendingParagraphIndex, pendingParagraphLookup ? "yes" : "no");
  } else {
    LOG_DBG("ERS", "Restored local pre-sync position: spine=%d page=%d paragraph=%u hasParagraph=%s", restoreSpineIndex,
            restorePage, pendingParagraphIndex, pendingParagraphLookup ? "yes" : "no");
  }

  // sync.totalPagesInSpine is the page count of the local spine at launch time.
  // When the restore targets a different spine, that count is meaningless for the
  // rescaling logic in render() and can cause out-of-bounds pages (the estimated
  // page number may exceed the local spine's count, producing progress > 1.0).
  // Store 0 to disable rescaling; the paragraph lookup handles precise positioning.
  const int restorePageCount = (restoreSpineIndex == sync.spineIndex) ? sync.totalPagesInSpine : 0;

  // Transient write — the next render's saveProgress() supplies the real percent before the user
  // can return to the home screen, so a placeholder 0 here is harmless.
  if (writeReaderProgressCache(epub->getCachePath(), restoreSpineIndex, restorePage, restorePageCount, 0)) {
    cachedSpineIndex = restoreSpineIndex;
    cachedChapterTotalPageCount = restorePageCount;
    LOG_DBG("ERS", "Prepared progress.bin for sync restore: spine=%d page=%d/%d", restoreSpineIndex, restorePage,
            sync.totalPagesInSpine);
  } else {
    // Fall back to directly seeding live state if cache write fails.
    currentSpineIndex = restoreSpineIndex;
    nextPageNumber = restorePage;
    cachedSpineIndex = restoreSpineIndex;
    cachedChapterTotalPageCount = restorePageCount;
  }

  sync.clear();
  APP_STATE.saveToFile();
  logReaderMemSnapshot("after_apply_pending_sync_session");
}

void EpubReaderActivity::applyPendingBookmarkJump() {
  auto& jump = APP_STATE.pendingBookmarkJump;
  if (!jump.active || !epub || jump.bookPath != epub->getPath()) {
    return;
  }
  LOG_DBG("ERS", "Applying pending bookmark jump: spine=%u page=%u", jump.spineIndex, jump.pageNumber);
  // Transient write before initializeReader; saveProgress() overwrites with the real percent.
  if (writeReaderProgressCache(epub->getCachePath(), jump.spineIndex, jump.pageNumber, 0, 0)) {
    cachedSpineIndex = jump.spineIndex;
    cachedChapterTotalPageCount = 0;
  } else {
    currentSpineIndex = jump.spineIndex;
    nextPageNumber = jump.pageNumber;
    cachedSpineIndex = jump.spineIndex;
    cachedChapterTotalPageCount = 0;
  }
  jump.clear();
  APP_STATE.saveToFile();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::applyTextDarkness(const uint8_t textDarkness) {
  if (SETTINGS.textDarkness == textDarkness) {
    return;
  }
  SETTINGS.textDarkness = textDarkness;
  SETTINGS.saveToFile();
  renderer.setTextDarkness(textDarkness);
  // Force a re-render so the new darkness is visible immediately.
  requestUpdate();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_LABELS)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

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

void EpubReaderActivity::applyBookReaderOverrides(const int8_t embeddedStyleOverride,
                                                  const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                                  const int8_t fontSizeOverride) {
  if (!epub) {
    return;
  }

  if (bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride &&
      bookFontFamilyOverride == fontFamilyOverride && bookFontSizeOverride == fontSizeOverride) {
    return;
  }

  bookEmbeddedStyleOverride = embeddedStyleOverride;
  bookImageRenderingOverride = imageRenderingOverride;
  bookFontFamilyOverride = fontFamilyOverride;
  bookFontSizeOverride = fontSizeOverride;
  RECENT_BOOKS.setReaderOverrides(epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride,
                                  bookFontFamilyOverride, bookFontSizeOverride);

  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();
}

bool EpubReaderActivity::getEffectiveEmbeddedStyle() const {
  if (bookEmbeddedStyleOverride >= 0) {
    return bookEmbeddedStyleOverride != 0;
  }
  return SETTINGS.embeddedStyle != 0;
}

uint8_t EpubReaderActivity::getEffectiveImageRendering() const {
  if (bookImageRenderingOverride >= 0) {
    return static_cast<uint8_t>(bookImageRenderingOverride);
  }
  return SETTINGS.imageRendering;
}

int EpubReaderActivity::getEffectiveReaderFontId() const {
  const uint8_t fontFamily =
      (bookFontFamilyOverride >= 0) ? static_cast<uint8_t>(bookFontFamilyOverride) : SETTINGS.fontFamily;
  const uint8_t fontSize = (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
  switch (fontFamily) {
    case CrossPointSettings::NOTOSANS:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return NOTOSANS_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case CrossPointSettings::LARGE:
          return NOTOSANS_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case CrossPointSettings::OPENDYSLEXIC:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case CrossPointSettings::LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
    case CrossPointSettings::BOOKERLY:
    default:
      switch (fontSize) {
        case CrossPointSettings::SMALL:
          return BOOKERLY_12_FONT_ID;
        case CrossPointSettings::MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case CrossPointSettings::LARGE:
          return BOOKERLY_16_FONT_ID;
        case CrossPointSettings::EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
  }
}

bool EpubReaderActivity::stepPageState(const bool isForwardTurn) {
  if (!epub || !section || section->pageCount == 0) {
    return false;
  }

  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
    } else {
      return false;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      RenderLock lock(*this);
      nextPageNumber = UINT16_MAX;
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
  }

  lastPageTurnTime = millis();
  return true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!stepPageState(isForwardTurn)) {
    return;
  }
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

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
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
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
  lastRenderStats = {};
  lastRenderStats.orientation = static_cast<uint8_t>(renderer.getOrientation());
  lastRenderStats.marginTop = orientedMarginTop;
  lastRenderStats.marginRight = orientedMarginRight;
  lastRenderStats.marginBottom = orientedMarginBottom;
  lastRenderStats.marginLeft = orientedMarginLeft;
  lastRenderStats.viewportWidth = viewportWidth;
  lastRenderStats.viewportHeight = viewportHeight;
  lastRenderStats.embeddedStyle = getEffectiveEmbeddedStyle();
  lastRenderStats.imageRendering = getEffectiveImageRendering();
  lastRenderStats.effectiveFontId = getEffectiveReaderFontId();
  lastRenderStats.textAntiAliasing = SETTINGS.textAntiAliasing;
  lastRenderStats.freeHeapBefore = esp_get_free_heap_size();
  lastRenderStats.largestFreeBlockBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);

  if (!section) {
    const bool embeddedStyle = lastRenderStats.embeddedStyle;
    const uint8_t imageRendering = lastRenderStats.imageRendering;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
    const unsigned long sectionStart = millis();

    if (!section->loadSectionFile(getEffectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
      LOG_DBG("ERS", "Cache not found, building...");
      lastRenderStats.cacheRebuilt = true;

      Rect popupRect{};
      const auto progressFn = [this, &popupRect](int progress) {
        if (popupRect.width == 0) {
          // Drawing the popup already does a full refresh, which serves as the
          // 0% indication; no need to follow it with a redundant fillPopupProgress.
          popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
          return;
        }
        GUI.fillPopupProgress(renderer, popupRect, progress);
      };

      if (!section->createSectionFile(getEffectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering,
                                      progressFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }
    lastRenderStats.sectionLoadMs = millis() - sectionStart;

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (pendingTocIndex) {
      if (const auto resolvedPage = section->getPageForTocIndex(*pendingTocIndex)) {
        section->currentPage = *resolvedPage;
      }
      pendingTocIndex.reset();
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // Resolve pending KOReader sync paragraph index to accurate page via Section paragraph LUT
    if (pendingParagraphLookup) {
      if (const auto page = section->getPageForParagraphIndex(pendingParagraphIndex)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved p[%u] to page %d (was %d)", pendingParagraphIndex, *page, nextPageNumber);
      } else {
        LOG_DBG("ERS", "Paragraph LUT not available, using estimated page %d", nextPageNumber);
      }
      pendingParagraphLookup = false;
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    // Safety clamp: estimated page numbers from sync or progress.bin may exceed
    // the actual page count when the section was built with different settings or
    // the estimate was based on a different spine's density.
    if (section->pageCount > 0 && section->currentPage >= section->pageCount) {
      LOG_DBG("ERS", "Clamping page %d to last page %d", section->currentPage, section->pageCount - 1);
      section->currentPage = section->pageCount - 1;
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

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    const unsigned long pageLoadStart = millis();
    auto p = section->loadPageFromSectionFile();
    lastRenderStats.pageLoadMs = millis() - pageLoadStart;
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);
    lastRenderStats.hadImages = p->hasImages();
    lastRenderStats.footnoteCount = static_cast<int>(currentPageFootnotes.size());
    lastRenderStats.spineIndex = currentSpineIndex;
    lastRenderStats.pageIndex = section->currentPage;
    lastRenderStats.pageCount = section->pageCount;

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    lastRenderStats.requestRenderMs = millis() - start;
    LOG_DBG("ERS", "Rendered page in %dms", lastRenderStats.requestRenderMs);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  lastRenderStats.freeHeapAfter = esp_get_free_heap_size();
  lastRenderStats.largestFreeBlockAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  lastRenderStats.valid = true;

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const bool embeddedStyle = getEffectiveEmbeddedStyle();
  const uint8_t imageRendering = getEffectiveImageRendering();

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(getEffectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(getEffectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  const uint8_t percent = epubProgressPercentByte(*epub, spineIndex, currentPage, pageCount);
  if (!writeReaderProgressCache(epub->getCachePath(), spineIndex, currentPage, pageCount, percent)) {
    LOG_ERR("ERS", "Could not save progress!");
    return;
  }
  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d (%d%%)", spineIndex, currentPage, percent);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  logReaderMemSnapshot("render_start");
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();
  logReaderMemSnapshot("prewarm_begin");

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  logReaderMemSnapshot("prewarm_end");

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;
  bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  lastRenderStats.imagePageWithAA = imagePageWithAA;
  lastRenderStats.forcedHalfRefresh = forceHalfRefreshThisPage;

  logReaderMemSnapshot("before_bw_render");
  page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  logReaderMemSnapshot("after_bw_render");

  if (imagePageWithAA) {
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
      page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
    if (forceHalfRefreshThisPage) {
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    }
  } else if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  logReaderMemSnapshot("bw_store_begin");
  renderer.storeBwBuffer();
  const auto tBwStore = millis();
  logReaderMemSnapshot("bw_store_end");

  if (page->hasImages() && getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    logReaderMemSnapshot("gray_lsb_begin");
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();
    logReaderMemSnapshot("gray_lsb_end");

    // Render and copy to MSB buffer
    logReaderMemSnapshot("gray_msb_begin");
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();
    logReaderMemSnapshot("gray_msb_end");

    // display grayscale part
    logReaderMemSnapshot("gray_display_begin");
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    fcm->logStats("gray");
    logReaderMemSnapshot("gray_display_end");

    // restore the bw data
    logReaderMemSnapshot("bw_restore_begin");
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();
    logReaderMemSnapshot("bw_restore_end");

    const auto tEnd = millis();
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.phases = {tPrewarm - t0,           tBwRender - tPrewarm,      tDisplay - tBwRender,
                              tBwStore - tDisplay,     tGrayLsb - tBwStore,       tGrayMsb - tGrayLsb,
                              tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0};
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data
    logReaderMemSnapshot("bw_restore_begin");
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();
    logReaderMemSnapshot("bw_restore_end");

    const auto tEnd = millis();
    lastRenderStats.usedGrayscale = false;
    lastRenderStats.phases = {
        tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, 0, 0, 0, tBwRestore - tBwStore,
        tEnd - t0};
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }

  if (const auto* cacheManager = renderer.getFontCacheManager()) {
    if (const auto* decompressor = cacheManager->getDecompressor()) {
      const auto& stats = decompressor->getStats();
      lastRenderStats.fontCacheHits = stats.cacheHits;
      lastRenderStats.fontCacheMisses = stats.cacheMisses;
      lastRenderStats.fontDecompressMs = stats.decompressTimeMs;
      lastRenderStats.fontUniqueGroups = stats.uniqueGroupsAccessed;
      lastRenderStats.fontPageBufferBytes = stats.pageBufferBytes;
      lastRenderStats.fontPageGlyphsBytes = stats.pageGlyphsBytes;
      lastRenderStats.fontPeakTempBytes = stats.peakTempBytes;
      lastRenderStats.fontGetBitmapTimeUs = stats.getBitmapTimeUs;
      lastRenderStats.fontGetBitmapCalls = stats.getBitmapCalls;
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;
  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    const int tocIndex =
        section ? section->getTocIndexForPage(section->currentPage) : epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex == -1) {
      title = tr(STR_UNNAMED);
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const bool isStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                      static_cast<uint16_t>(section->currentPage));
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, isStarred);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

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

bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) == 6) {
      spineIndex = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      pageNumber = (int)((uint32_t)data[2] | ((uint32_t)data[3] << 8));
    }
    f.close();
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  marginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(filePath);
  const uint8_t effectiveFontFamily =
      currentBook.fontFamilyOverride >= 0 ? static_cast<uint8_t>(currentBook.fontFamilyOverride) : SETTINGS.fontFamily;
  const uint8_t effectiveFontSize =
      currentBook.fontSizeOverride >= 0 ? static_cast<uint8_t>(currentBook.fontSizeOverride) : SETTINGS.fontSize;
  auto getEffectiveFontId = [&](uint8_t family, uint8_t size) {
    switch (family) {
      case CrossPointSettings::NOTOSANS:
        switch (size) {
          case CrossPointSettings::SMALL:
            return NOTOSANS_12_FONT_ID;
          case CrossPointSettings::LARGE:
            return NOTOSANS_16_FONT_ID;
          case CrossPointSettings::EXTRA_LARGE:
            return NOTOSANS_18_FONT_ID;
          case CrossPointSettings::MEDIUM:
          default:
            return NOTOSANS_14_FONT_ID;
        }
      case CrossPointSettings::OPENDYSLEXIC:
        switch (size) {
          case CrossPointSettings::SMALL:
            return OPENDYSLEXIC_8_FONT_ID;
          case CrossPointSettings::LARGE:
            return OPENDYSLEXIC_12_FONT_ID;
          case CrossPointSettings::EXTRA_LARGE:
            return OPENDYSLEXIC_14_FONT_ID;
          case CrossPointSettings::MEDIUM:
          default:
            return OPENDYSLEXIC_10_FONT_ID;
        }
      case CrossPointSettings::BOOKERLY:
      default:
        switch (size) {
          case CrossPointSettings::SMALL:
            return BOOKERLY_12_FONT_ID;
          case CrossPointSettings::LARGE:
            return BOOKERLY_16_FONT_ID;
          case CrossPointSettings::EXTRA_LARGE:
            return BOOKERLY_18_FONT_ID;
          case CrossPointSettings::MEDIUM:
          default:
            return BOOKERLY_14_FONT_ID;
        }
    }
  };

  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  if (!section->loadSectionFile(getEffectiveFontId(effectiveFontFamily, effectiveFontSize),
                                SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.embeddedStyle, SETTINGS.imageRendering)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(getEffectiveFontId(effectiveFontFamily, effectiveFontSize),
                                    SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering)) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  renderer.clearScreen();
  page->render(renderer, getEffectiveFontId(effectiveFontFamily, effectiveFontSize), marginLeft, marginTop);
  // No displayBuffer call — caller (SleepActivity) handles that after compositing the overlay
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  const bool isCurrentPageStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                                 static_cast<uint16_t>(section->currentPage));
  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookFontSizeOverride, SETTINGS.textDarkness, !bookmarkStore.isEmpty(), isCurrentPageStarred),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        applyTextDarkness(menu.textDarkness);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                                 menu.fontSizeOverride);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void EpubReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      pageTurn(true);
      break;
    case BA::BTN_PAGE_BACK:
      pageTurn(false);
      break;
    case BA::BTN_PAGE_FORWARD_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(true)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_PAGE_BACK_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(false)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_STAR_PAGE:
      if (section) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    case BA::BTN_FOOTNOTES:
      if (!currentPageFootnotes.empty()) {
        if (currentPageFootnotes.size() == 1) {
          navigateToHref(currentPageFootnotes[0].href, true);
        } else {
          startActivityForResult(
              std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                  navigateToHref(footnoteResult.href, true);
                }
              });
        }
      }
      break;
    case BA::BTN_OPEN_TOC:
      if (epub) {
        const int spineIdx = currentSpineIndex;
        const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                   : epub->getTocIndexForSpineIndex(currentSpineIndex);
        ReaderUtils::enforceExitFullRefresh(renderer);
        startActivityForResult(std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub,
                                                                                    epub->getPath(), spineIdx, tocIdx),
                               [this](const ActivityResult& result) {
                                 if (result.isCancelled) return;
                                 RenderLock lock(*this);
                                 const auto& chapter = std::get<ChapterResult>(result.data);
                                 auto resolvedPage =
                                     (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                         ? section->getPageForTocIndex(*chapter.tocIndex)
                                         : std::nullopt;
                                 if (resolvedPage) {
                                   section->currentPage = *resolvedPage;
                                 } else {
                                   pendingTocIndex = chapter.tocIndex;
                                   currentSpineIndex = chapter.spineIndex;
                                   nextPageNumber = 0;
                                   section.reset();
                                 }
                               });
      }
      break;
    case BA::BTN_NEXT_SECTION:
    case BA::BTN_PREV_SECTION: {
      const bool forward = (action == BA::BTN_NEXT_SECTION);
      {
        RenderLock lock(*this);
        if (section && section->pageCount > 0) {
          const int curTocIndex = section->getTocIndexForPage(section->currentPage);
          const int nextTocIndex = forward ? curTocIndex + 1 : curTocIndex - 1;
          if (curTocIndex < 0) {
            nextPageNumber = 0;
            currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
            section.reset();
          } else if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
            const int newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);
            if (newSpineIndex == currentSpineIndex) {
              if (const auto resolvedPage = section->getPageForTocIndex(nextTocIndex)) {
                section->currentPage = *resolvedPage;
              }
            } else {
              pendingTocIndex = nextTocIndex;
              nextPageNumber = 0;
              currentSpineIndex = newSpineIndex;
              section.reset();
            }
          } else if (forward) {
            nextPageNumber = 0;
            currentSpineIndex = epub->getSpineItemsCount();
            section.reset();
          } else {
            nextPageNumber = 0;
            currentSpineIndex = epub->getTocItem(curTocIndex).spineIndex - 1;
            section.reset();
          }
        } else {
          nextPageNumber = 0;
          currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
      }
      requestUpdate();
      break;
    }
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      finish();
      break;
    case BA::BTN_READER_MENU:
      if (epub) {
        openReaderMenu();
      }
      break;
    case BA::BTN_KOREADER_SYNC:
      launchKOReaderSync(SyncLaunchMode::COMPARE);
      break;
    default:
      break;
  }
}
