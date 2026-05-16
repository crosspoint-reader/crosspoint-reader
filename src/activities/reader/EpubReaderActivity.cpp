#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderProgressPolicy.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/StatusPageInfo.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

IncrementalSection::LayoutCacheKey makeLayoutCacheKey(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  IncrementalSection::LayoutCacheKey key;
  key.cacheVersion = IncrementalSection::CACHE_VERSION;
  key.fontId = SETTINGS.getReaderFontId();
  key.lineCompression = SETTINGS.getReaderLineCompression();
  key.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  key.paragraphAlignment = SETTINGS.paragraphAlignment;
  key.viewportWidth = viewportWidth;
  key.viewportHeight = viewportHeight;
  key.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  key.embeddedStyle = SETTINGS.embeddedStyle;
  key.imageRendering = SETTINGS.imageRendering;
  key.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  return key;
}

IncrementalBuildOptions makeBuildOptions(const uint16_t viewportWidth, const uint16_t viewportHeight,
                                         const std::function<void()>& popupFn = nullptr) {
  IncrementalBuildOptions options;
  options.fontId = SETTINGS.getReaderFontId();
  options.lineCompression = SETTINGS.getReaderLineCompression();
  options.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  options.paragraphAlignment = SETTINGS.paragraphAlignment;
  options.viewportWidth = viewportWidth;
  options.viewportHeight = viewportHeight;
  options.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  options.embeddedStyle = SETTINGS.embeddedStyle;
  options.imageRendering = SETTINGS.imageRendering;
  options.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  options.popupFn = popupFn;
  return options;
}

IncrementalBuildBudget indexBudget(const IncrementalBuildBudgetProfile profile) {
  return IncrementalBuildBudgets::forProfile(profile);
}

IncrementalBuildBudget responsiveBackgroundBudget(const IncrementalBuildBudgetProfile profile) {
  auto budget = indexBudget(profile);
  budget.maxInputChunks = (profile == IncrementalBuildBudgetProfile::NextPrewarm)
                              ? EpubIndexingPolicy::nextPrewarmInputChunks(1)
                              : EpubIndexingPolicy::indexBatchInputChunks(1);
  budget.maxCompletedPages = 1;
  budget.maxMillis = EpubIndexingPolicy::indexBatchMaxMillis(1);
  return budget;
}

uint32_t displayPageCount(const SectionHandle* section) {
  if (!section) {
    return 0;
  }
  return section->isComplete() ? section->finalPageCount() : section->knownPageCount();
}

uint16_t persistablePageCountForSection(const SectionHandle* section) {
  return ReaderProgressPolicy::persistablePageCount(section && section->isComplete(),
                                                    section ? section->finalPageCount() : 0);
}

bool sectionPumpDidRun(const SectionPumpResult& result) {
  return result.status == SectionPumpStatus::Pumped || result.status == SectionPumpStatus::Complete ||
         result.status == SectionPumpStatus::Failed;
}

enum class SectionPumpGoal {
  PageAvailable,
  InitialWindowFilled,
  OutrunWindowFilled,
  SectionComplete,
  AnchorResolved,
};

struct SectionPumpGoalSpec {
  SectionPumpGoal goal = SectionPumpGoal::SectionComplete;
  uint32_t pageNumber = 0;
  const std::string* anchor = nullptr;
};

struct SectionPumpLoopResult {
  SectionPumpStatus status = SectionPumpStatus::NoWork;
  uint32_t pagesBefore = 0;
  uint32_t pagesAfter = 0;
  bool pumped = false;
  bool goalMet = false;
};

bool sectionPumpGoalMet(const SectionHandle& section, const SectionPumpGoalSpec& spec) {
  switch (spec.goal) {
    case SectionPumpGoal::PageAvailable:
      return section.hasPage(spec.pageNumber);
    case SectionPumpGoal::InitialWindowFilled:
      return !EpubIndexingPolicy::initialIndexWindowNeedsPages(section.knownPageCount(), section.isComplete());
    case SectionPumpGoal::OutrunWindowFilled:
      return section.hasPage(spec.pageNumber) && !EpubIndexingPolicy::outrunIndexWindowNeedsPages(
                                                     spec.pageNumber, section.knownPageCount(), section.isComplete());
    case SectionPumpGoal::SectionComplete:
      return section.isComplete();
    case SectionPumpGoal::AnchorResolved:
      return spec.anchor && section.getPageForAnchor(*spec.anchor).has_value();
  }
  return false;
}

SectionPumpLoopResult pumpSectionUntil(SectionHandle& section, const SectionPumpGoalSpec& goal,
                                       const IncrementalBuildBudgetProfile budgetProfile) {
  SectionPumpLoopResult loopResult;
  loopResult.pagesBefore = section.knownPageCount();
  loopResult.goalMet = sectionPumpGoalMet(section, goal);

  while (!loopResult.goalMet && section.mode() == SectionHandleMode::IncrementalBuilding) {
    const auto pumpResult = section.pump(indexBudget(budgetProfile));
    loopResult.status = pumpResult.status;
    loopResult.pagesAfter = pumpResult.pagesAfter;
    loopResult.pumped = loopResult.pumped || sectionPumpDidRun(pumpResult);
    loopResult.goalMet = sectionPumpGoalMet(section, goal);

    if (pumpResult.status == SectionPumpStatus::DeferredLowMemory || pumpResult.status == SectionPumpStatus::Failed ||
        pumpResult.status == SectionPumpStatus::Complete || pumpResult.status == SectionPumpStatus::NoWork) {
      break;
    }
  }

  loopResult.goalMet = sectionPumpGoalMet(section, goal);
  loopResult.pagesAfter = section.knownPageCount();
  return loopResult;
}

bool ensureOutrunWindowAvailable(SectionHandle& section, const uint32_t pageNumber) {
  const auto result = pumpSectionUntil(section, {SectionPumpGoal::OutrunWindowFilled, pageNumber, nullptr},
                                       IncrementalBuildBudgetProfile::Outrun);
  return result.goalMet || section.hasPage(pageNumber);
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
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

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  nextSectionPrewarm.reset();
  backgroundIndexingWorkActive = false;
  epub.reset();
}

bool EpubReaderActivity::hasCurrentIndexingWork() const {
  return section && section->hasActiveBuilder() && !section->isComplete();
}

bool EpubReaderActivity::hasPrewarmIndexingWork() const {
  return nextSectionPrewarm && nextSectionPrewarm->hasActiveBuilder() && !nextSectionPrewarm->isComplete();
}

bool EpubReaderActivity::hasBackgroundIndexingWork() const {
  if (RenderLock::peek()) {
    return true;
  }
  return backgroundIndexingWorkActive;
}

bool EpubReaderActivity::skipLoopDelay() { return hasBackgroundIndexingWork(); }

bool EpubReaderActivity::preventAutoSleep() { return hasBackgroundIndexingWork(); }

bool EpubReaderActivity::pumpBackgroundIndexing() {
  if (!epub || !section) {
    return false;
  }

  const bool currentWasActive = hasCurrentIndexingWork();
  const bool prewarmWasActive = hasPrewarmIndexingWork();
  bool didWork = false;

  if (currentWasActive) {
    const auto result = section->pump(responsiveBackgroundBudget(IncrementalBuildBudgetProfile::CurrentBackground));
    didWork = sectionPumpDidRun(result);
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
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
  didWork = maintainPrewarmWindow(viewportWidth, viewportHeight) || didWork;

  const bool currentNowActive = hasCurrentIndexingWork();
  const bool prewarmNowActive = hasPrewarmIndexingWork();
  backgroundIndexingWorkActive = currentNowActive || prewarmNowActive;
  if (currentWasActive != currentNowActive || prewarmWasActive != prewarmNowActive) {
    requestUpdate();
  }

  return didWork || currentWasActive || prewarmWasActive;
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (pendingPageTurnDirection != 0 && !RenderLock::peek()) {
    const bool queuedForwardTurn = pendingPageTurnDirection > 0;
    pendingPageTurnDirection = 0;
    if (!section) {
      requestUpdate();
      return;
    }
    pageTurn(queuedForwardTurn);
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

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int currentPage = section ? section->currentPage + 1 : 0;
    const int totalPages = section ? static_cast<int>(displayPageCount(section.get())) : 0;
    float bookProgress = 0.0f;
    const uint32_t pageCount = displayPageCount(section.get());
    if (epub->getBookSize() > 0 && section && pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty()),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             toggleAutoPageTurn(menu.pageTurnOption);
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    // Loop-side background indexing waits for render ownership to clear.
    if (!RenderLock::peek()) {
      pumpBackgroundIndexing();
    }
    return;
  }

  if (RenderLock::peek()) {
    pendingPageTurnDirection = nextTriggered ? 1 : -1;
    LOG_DBG("ERS", "Queued page turn while render/indexing is active");
    lastPageTurnTime = millis();
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
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
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
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
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
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
      const uint32_t pageCount = displayPageCount(section.get());
      if (epub && epub->getBookSize() > 0 && section && pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(pageCount);
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
      if (section && section->currentPage >= 0 && section->hasPage(static_cast<uint32_t>(section->currentPage))) {
        auto p = section->loadPage(static_cast<uint32_t>(section->currentPage));
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
          uint16_t backupPageCount = persistablePageCountForSection(section.get());
          section.reset();
          nextSectionPrewarm.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
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
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages =
            section ? static_cast<int>(displayPageCount(section.get())) : cachedChapterTotalPageCount;
        const int persistableTotalPages =
            section ? static_cast<int>(persistablePageCountForSection(section.get())) : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && section->hasPage(static_cast<uint32_t>(currentPage))) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, persistableTotalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release Epub and Section to free ~65KB RAM for the TLS handshake.
        LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
          nextSectionPrewarm.reset();
          epub.reset();
        }
        LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
  }
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
      cachedChapterTotalPageCount = static_cast<int>(persistablePageCountForSection(section.get()));
      nextPageNumber = section->currentPage;
    }
    nextSectionPrewarm.reset();

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = static_cast<int>(persistablePageCountForSection(section.get()));
      nextPageNumber = section->currentPage;
    }
    nextSectionPrewarm.reset();
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    const uint32_t targetPage = static_cast<uint32_t>(section->currentPage + 1);
    const bool targetAvailable =
        section->hasPage(targetPage) || (!section->isComplete() && ensureOutrunWindowAvailable(*section, targetPage));
    if (targetAvailable) {
      section->currentPage++;
    } else if (section->isComplete()) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    } else if (section->mode() == SectionHandleMode::MissingOrFailed) {
      section.reset();
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
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
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
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
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

  if (!section && nextSectionPrewarm && nextSectionPrewarm->spineIndex() == currentSpineIndex) {
    LOG_DBG("ERS", "Adopting prewarmed section: %d", currentSpineIndex);
    section = std::move(nextSectionPrewarm);
  } else if (nextSectionPrewarm &&
             !EpubIndexingPolicy::prewarmMatchesSpine(nextSectionPrewarm->spineIndex(), currentSpineIndex + 1)) {
    nextSectionPrewarm.reset();
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
    section = SectionHandle::openOrCreate(epub, currentSpineIndex, renderer,
                                          makeLayoutCacheKey(viewportWidth, viewportHeight),
                                          makeBuildOptions(viewportWidth, viewportHeight, popupFn));
    if (!section || section->mode() == SectionHandleMode::MissingOrFailed) {
      LOG_ERR("ERS", "Failed to open or build section data");
      section.reset();
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      showPendingSyncSaveError();
      return;
    }

    if (!section->hasPage(0) && !section->isComplete()) {
      GUI.drawPopup(renderer, tr(STR_INDEXING));
      const auto firstPageResult = pumpSectionUntil(*section, {SectionPumpGoal::PageAvailable, 0, nullptr},
                                                    IncrementalBuildBudgetProfile::InitialIndex);
      if (!firstPageResult.goalMet) {
        if (section->mode() == SectionHandleMode::MissingOrFailed) {
          LOG_ERR("ERS", "Failed to build first page");
          section.reset();
          renderer.clearScreen();
          renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
          renderer.displayBuffer();
        } else {
          requestUpdate();
        }
        showPendingSyncSaveError();
        return;
      }
    }

    if (EpubIndexingPolicy::initialIndexWindowNeedsPages(section->knownPageCount(), section->isComplete())) {
      const auto initialWindowResult = pumpSectionUntil(*section, {SectionPumpGoal::InitialWindowFilled, 0, nullptr},
                                                        IncrementalBuildBudgetProfile::InitialIndex);
      if (initialWindowResult.status == SectionPumpStatus::DeferredLowMemory) {
        requestUpdate();
      }
      if (section->mode() == SectionHandleMode::MissingOrFailed) {
        section.reset();
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump == std::numeric_limits<uint16_t>::max()) {
        const auto lastPageResult = pumpSectionUntil(*section, {SectionPumpGoal::SectionComplete, 0, nullptr},
                                                     IncrementalBuildBudgetProfile::Outrun);
        if (lastPageResult.status == SectionPumpStatus::DeferredLowMemory) {
          requestUpdate();
          showPendingSyncSaveError();
          return;
        }
        section->currentPage = section->finalPageCount() > 0 ? static_cast<int>(section->finalPageCount() - 1) : 0;
      } else if (section->isComplete() && *pendingPageJump >= section->finalPageCount() &&
                 section->finalPageCount() > 0) {
        section->currentPage = static_cast<int>(section->finalPageCount() - 1);
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->isComplete() && section->currentPage >= static_cast<int>(section->finalPageCount()) &&
                 section->finalPageCount() > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %lu", section->currentPage,
                static_cast<unsigned long>(section->finalPageCount() - 1));
        section->currentPage = static_cast<int>(section->finalPageCount() - 1);
      }
    }

    if (!pendingAnchor.empty()) {
      const auto anchorResult = pumpSectionUntil(*section, {SectionPumpGoal::AnchorResolved, 0, &pendingAnchor},
                                                 IncrementalBuildBudgetProfile::Outrun);
      if (anchorResult.status == SectionPumpStatus::DeferredLowMemory) {
        requestUpdate();
        showPendingSyncSaveError();
        return;
      }
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      auto remapDecision = ReaderProgressPolicy::decideResumeRemap(
          currentSpineIndex, cachedSpineIndex, section->currentPage, static_cast<uint32_t>(cachedChapterTotalPageCount),
          section->isComplete(), section->finalPageCount());
      if (remapDecision.deferUntilComplete) {
        const auto remapResult = pumpSectionUntil(*section, {SectionPumpGoal::SectionComplete, 0, nullptr},
                                                  IncrementalBuildBudgetProfile::Outrun);
        if (remapResult.status == SectionPumpStatus::DeferredLowMemory || !section->isComplete()) {
          requestUpdate();
          showPendingSyncSaveError();
          return;
        }
        remapDecision = ReaderProgressPolicy::decideResumeRemap(
            currentSpineIndex, cachedSpineIndex, section->currentPage,
            static_cast<uint32_t>(cachedChapterTotalPageCount), section->isComplete(), section->finalPageCount());
      }
      if (remapDecision.applyPage) {
        section->currentPage = remapDecision.pageNumber;
      }
      if (!remapDecision.deferUntilComplete) {
        cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
      }
    }

    if (pendingPercentJump) {
      const auto percentJumpResult = pumpSectionUntil(*section, {SectionPumpGoal::SectionComplete, 0, nullptr},
                                                      IncrementalBuildBudgetProfile::Outrun);
      if (percentJumpResult.status == SectionPumpStatus::DeferredLowMemory) {
        requestUpdate();
        showPendingSyncSaveError();
        return;
      }
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->finalPageCount()));
      if (newPage >= static_cast<int>(section->finalPageCount())) {
        newPage = section->finalPageCount() > 0 ? static_cast<int>(section->finalPageCount() - 1) : 0;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->knownPageCount() == 0 && section->isComplete()) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0) {
    LOG_DBG("ERS", "Page out of bounds: %d", section->currentPage);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (!section->hasPage(static_cast<uint32_t>(section->currentPage))) {
    if (!ensureOutrunWindowAvailable(*section, static_cast<uint32_t>(section->currentPage))) {
      if (section->mode() != SectionHandleMode::MissingOrFailed && !section->isComplete()) {
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }
      LOG_DBG("ERS", "Page unavailable: %d (known %lu)", section->currentPage,
              static_cast<unsigned long>(section->knownPageCount()));
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }
  }

  {
    auto p = section->loadPage(static_cast<uint32_t>(section->currentPage));
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  saveProgress(currentSpineIndex, section->currentPage,
               static_cast<int>(persistablePageCountForSection(section.get())));

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

bool EpubReaderActivity::maintainPrewarmWindow(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || !section->isComplete()) {
    return false;
  }

  if (!EpubIndexingPolicy::shouldPrewarmNextChapter(static_cast<uint32_t>(std::max(section->currentPage, 0)),
                                                    section->finalPageCount(), section->isComplete())) {
    return false;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return false;
  }

  bool changed = false;
  if (nextSectionPrewarm && nextSectionPrewarm->spineIndex() != nextSpineIndex) {
    nextSectionPrewarm.reset();
    changed = true;
  }

  if (!nextSectionPrewarm) {
    nextSectionPrewarm =
        SectionHandle::openOrCreate(epub, nextSpineIndex, renderer, makeLayoutCacheKey(viewportWidth, viewportHeight),
                                    makeBuildOptions(viewportWidth, viewportHeight));
    if (!nextSectionPrewarm || nextSectionPrewarm->mode() == SectionHandleMode::MissingOrFailed) {
      LOG_DBG("ERS", "Next section prewarm open failed: %d", nextSpineIndex);
      nextSectionPrewarm.reset();
      return changed;
    }
    LOG_DBG("ERS", "Next section prewarm opened: %d", nextSpineIndex);
    changed = true;
  }

  if (nextSectionPrewarm->hasActiveBuilder()) {
    const auto result =
        nextSectionPrewarm->pump(responsiveBackgroundBudget(IncrementalBuildBudgetProfile::NextPrewarm));
    changed = sectionPumpDidRun(result) || changed;
    if (result.status == SectionPumpStatus::Failed) {
      LOG_DBG("ERS", "Next section prewarm failed: %d", nextSpineIndex);
      nextSectionPrewarm.reset();
      changed = true;
    }
  }

  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
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
  const bool bwStored = renderer.storeBwBuffer();
  const auto tBwStore = millis();

  if (SETTINGS.textAntiAliasing && !bwStored) {
    LOG_ERR("ERS", "Skipping EPUB text grayscale AA: failed to store BW buffer");
    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "aa=skipped total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tEnd - t0);
    return;
  }

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
  const uint32_t pageCount = displayPageCount(section.get());
  const float sectionChapterProg =
      (pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount)) : 0.0f;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;
  StatusPageInfo pageInfo;
  pageInfo.currentPage = section->currentPage >= 0 ? static_cast<uint32_t>(section->currentPage) : 0;
  pageInfo.totalPages = pageCount;
  pageInfo.totalKnown = section->isComplete();
  pageInfo.hasMorePages = !section->isComplete() && section->knownPageCount() > 0;
  pageInfo.futureIndexingActive =
      nextSectionPrewarm && nextSectionPrewarm->hasActiveBuilder() && !nextSectionPrewarm->isComplete();

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

  GUI.drawStatusBar(renderer, bookProgress, pageInfo, title, 0, textYOffset);
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

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    const uint32_t pageCount = displayPageCount(section.get());
    info.totalPages = static_cast<int>(pageCount);
    if (epub && epub->getBookSize() > 0 && pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
