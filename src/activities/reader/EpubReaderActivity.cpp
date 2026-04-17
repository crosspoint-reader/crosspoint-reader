#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
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

void logReaderMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}

bool writeReaderProgressCache(const std::string& cachePath, const int spineIndex, const int currentPage,
                              const int pageCount) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + "/progress.bin", f)) {
    LOG_ERR("ERS", "Failed to open progress cache for sync restore: %s", cachePath.c_str());
    return false;
  }

  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  f.write(data, 6);
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

  epub->setupCacheDir();
  applyPendingSyncSession();
  applyPendingBookmarkJump();

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

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());

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

  // Trigger first update
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
    return;
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

  // Long press CONFIRM (1s+) goes directly to KOReader sync when credentials are configured.
  // We intentionally keep long-press on the richer compare flow so advanced
  // conflict-resolution behavior stays available even after simplifying menu UX.
  // Without credentials, fall through to the regular menu on release.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && KOREADER_STORE.hasCredentials()) {
    launchKOReaderSync(SyncLaunchMode::COMPARE);
    return;
  }

  // Short press CONFIRM enters reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
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
            !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, SETTINGS.textDarkness,
            !bookmarkStore.isEmpty(), isCurrentPageStarred),
        [this](const ActivityResult& result) {
          // Always apply orientation/darkness change even if the menu was cancelled
          const auto& menu = std::get<MenuResult>(result.data);
          applyOrientation(menu.orientation);
          applyTextDarkness(menu.textDarkness);
          toggleAutoPageTurn(menu.pageTurnOption);
          applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride);
          if (!result.isCancelled) {
            onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
          }
        });
  }

  // Long press BACK (1s+) goes to home screen
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    ReaderUtils::enforceExitFullRefresh(renderer);
    onGoHome();
    return;
  }

  // Short press BACK returns to the calling activity (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    ReaderUtils::enforceExitFullRefresh(renderer);
    finish();
    return;
  }

  const bool screenshotChordReleased = gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) && !screenshotChordReleased) {
    if (currentPageFootnotes.size() == 1) {
      navigateToHref(currentPageFootnotes[0].href, true);
    } else if (currentPageFootnotes.size() > 1) {
      ReaderUtils::enforceExitFullRefresh(renderer);
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
    }
    return;
  }

  // Star page toggle via short power button press
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::STAR_PAGE &&
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
      bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
      requestUpdate();
    }
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
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

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  // Chapter skip navigates by TOC entries, not spine boundaries.
  // Spine items without their own TOC entry inherit the previous spine's tocIndex
  // (see BookMetadataCache), so they're treated as continuations of the last chapter.
  // At the boundaries: skipping forward past the last TOC entry jumps to end-of-book
  // (clamped in render()); skipping backward before the first TOC entry jumps to the
  // spine before the current chapter's first spine (clamped to 0 in render()).
  if (skipChapter) {
    lastPageTurnTime = millis();
    {
      RenderLock lock(*this);

      if (section && section->pageCount > 0) {
        const int curTocIndex = section->getTocIndexForPage(section->currentPage);
        const int nextTocIndex = nextTriggered ? curTocIndex + 1 : curTocIndex - 1;

        if (curTocIndex < 0) {
          // No TOC entry for this spine, fall back to spine-level skip
          nextPageNumber = 0;
          currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        } else if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
          const int newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);

          if (newSpineIndex == currentSpineIndex) {
            if (const auto resolvedPage = section->getPageForTocIndex(nextTocIndex)) {
              section->currentPage = *resolvedPage;
            } else {
              LOG_DBG("ERS", "No page boundary for TOC %d in spine %d, staying on current page", nextTocIndex,
                      currentSpineIndex);
            }
          } else {
            pendingTocIndex = nextTocIndex;
            nextPageNumber = 0;
            currentSpineIndex = newSpineIndex;
            section.reset();
          }
        } else if (nextTriggered) {
          // Beyond last TOC entry, go to end of book
          nextPageNumber = 0;
          currentSpineIndex = epub->getSpineItemsCount();
          section.reset();
        } else {
          // Before first TOC entry, skip to spine before the current chapter
          nextPageNumber = 0;
          currentSpineIndex = epub->getTocItem(curTocIndex).spineIndex - 1;
          section.reset();
        }
      } else {
        nextPageNumber = 0;
        currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
        section.reset();
      }
    }
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
  sync.paragraphIndex = 0;
  sync.hasParagraphIndex = false;
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

  if (writeReaderProgressCache(epub->getCachePath(), restoreSpineIndex, restorePage, restorePageCount)) {
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
  if (writeReaderProgressCache(epub->getCachePath(), jump.spineIndex, jump.pageNumber, 0)) {
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
                                                  const int8_t imageRenderingOverride) {
  if (!epub) {
    return;
  }

  if (bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride) {
    return;
  }

  bookEmbeddedStyleOverride = embeddedStyleOverride;
  bookImageRenderingOverride = imageRenderingOverride;
  RECENT_BOOKS.setReaderOverrides(epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride);

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

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
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
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
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

  if (!section) {
    const bool embeddedStyle = getEffectiveEmbeddedStyle();
    const uint8_t imageRendering = getEffectiveImageRendering();
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
      LOG_DBG("ERS", "Cache not found, building...");

      Rect popupRect{};
      const auto progressFn = [this, &popupRect](int progress) {
        if (popupRect.width == 0) {
          popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
        }
        GUI.fillPopupProgress(renderer, popupRect, progress);
      };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
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
    auto p = section->loadPageFromSectionFile();
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

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);

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
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, imageRendering)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    data[0] = spineIndex & 0xFF;
    data[1] = (spineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();

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
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();
  const auto tBwStore = millis();

  // grayscale rendering
  // TODO: Only do this if font supports it
  if (SETTINGS.textAntiAliasing) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    fcm->logStats("gray");

    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
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
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                SETTINGS.imageRendering)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                    SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                    viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                    SETTINGS.imageRendering)) {
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
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
  // No displayBuffer call — caller (SleepActivity) handles that after compositing the overlay
  return true;
}
