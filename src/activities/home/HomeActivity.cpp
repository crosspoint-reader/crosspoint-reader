#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/SourceIdentityStore.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <array>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/home/DashboardProgress.h"
#include "activities/home/DashboardStatsPolicy.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressFileCodec.h"
#include "activities/reader/ReadingStatsActivity.h"
#include "activities/reader/ReadingStatsCompletionTransaction.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/themes/crossvi/CrossViTheme.h"
#include "fontIds.h"

namespace {
bool hasUsableBitmapThumbnail(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  return bitmap.parseHeaders() == BmpReaderError::Ok;
}

struct DashboardProgressValidationContext {
  const std::string* cachePath = nullptr;
  int spineCount = 0;
};

bool validateDashboardProgressCandidate(const uint8_t* data, const size_t size, const void* rawContext) {
  if (!rawContext) return false;
  const auto& context = *static_cast<const DashboardProgressValidationContext*>(rawContext);
  if (!context.cachePath) return false;

  DashboardProgress::Position position;
  return DashboardProgress::decode(data, size, position) &&
         DashboardProgress::validate(
             position, context.spineCount,
             Section::getCachedPageCount(*context.cachePath, static_cast<int>(position.spineIndex)));
}

StrId homeBookHintLabelId(const HomeBookSummary& summary) {
  switch (homeBookAction(summary)) {
    case HomeBookAction::Continue:
      return StrId::STR_CONTINUE;
    case HomeBookAction::ReadAgain:
      return StrId::STR_READ_AGAIN;
    case HomeBookAction::Start:
      return StrId::STR_START;
    case HomeBookAction::Open:
    default:
      return StrId::STR_OPEN;
  }
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  return HomeMenuMapping::selectionCount(static_cast<int>(recentBooks.size()), hasOpdsServers,
                                         hasReadingStatsShortcut());
}

bool HomeActivity::hasReadingStatsShortcut() const {
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI) return true;
  if (recentBooks.empty()) return false;
  const std::string& path = recentBooks.front().path;
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

bool HomeActivity::hasHomeReadingSummary() const {
  return SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadBookSummary() {
  bookSummary = {};
  bookSummary.dailyReadingGoalMinutes = SETTINGS.dailyReadingGoalMinutes;
  readingStatsPresentation.reset();

  // The CrossVi Today/Goal card describes this device, not the latest book,
  // so keep it useful for a new library as well. Other themes retain their
  // existing empty-state behavior.
  if (recentBooks.empty()) {
    if (SETTINGS.uiTheme != CrossPointSettings::UI_THEME::CROSSVI) return;

    GlobalReadingStats::LoadStatus deviceStatus = GlobalReadingStats::LoadStatus::Missing;
    const GlobalReadingStats deviceStats = GlobalReadingStats::load(&deviceStatus);
    const bool deviceStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(deviceStatus);
    const bool hasSyncedDirectory = GlobalReadingStats::hasSyncedStats();
    const GlobalReadingStatsAggregation allSyncedStats = hasSyncedDirectory && deviceStatsTrusted
                                                             ? GlobalReadingStats::loadAggregatedWithReport(deviceStats)
                                                             : GlobalReadingStatsAggregation{};

    ReadingStatsDateTime now;
    const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
    if (currentDateTime && deviceStatsTrusted) {
      bookSummary.hasTodayReadingSeconds = deviceStats.readingSummaryForDate(
          now.date, bookSummary.todayReadingSeconds, bookSummary.todayReadingSessions);
    }

    const BookReadingStats noBookStats;
    readingStatsPresentation = buildReadingStatsPresentation(
        noBookStats, false, deviceStats, deviceStatsTrusted, allSyncedStats, currentDateTime,
        ReadingStatsMetric::notApplicable(), false);
    return;
  }

  GlobalReadingStats::LoadStatus localStatus = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats localStats = GlobalReadingStats::load(&localStatus);
  const bool localStatsTrusted = GlobalReadingStats::isTrustedLoadStatus(localStatus);
  const bool hasSyncedDirectory = GlobalReadingStats::hasSyncedStats();
  GlobalReadingStatsAggregation syncedReport;
  if (hasSyncedDirectory && localStatsTrusted) {
    syncedReport = GlobalReadingStats::loadAggregatedWithReport(localStats);
  }

  DashboardStatsPolicyInput policyInput;
  policyInput.localStatsTrusted = localStatsTrusted;
  policyInput.localStatsMissing = localStatus == GlobalReadingStats::LoadStatus::Missing;
  policyInput.hasSyncedDirectory = hasSyncedDirectory;
  policyInput.syncedScanComplete = syncedReport.scanComplete;
  policyInput.validPeerCount = syncedReport.validPeerCount;
  policyInput.skippedPeerCount = syncedReport.skippedPeerCount;
  policyInput.isEpub = FsHelpers::hasEpubExtension(recentBooks[0].path);

  const auto applyPolicy = [&]() {
    const DashboardStatsPolicyResult policy = DashboardStatsPolicy::evaluate(policyInput);
    bookSummary.bookStatsState = policy.bookStats;
    bookSummary.globalStatsState = policy.globalStats;
    bookSummary.syncedStatsState = policy.syncedStats;
    bookSummary.usingSyncedStats = policy.useAllSynced;
    bookSummary.syncedDeviceCount = policy.useAllSynced && syncedReport.validPeerCount != UINT16_MAX
                                        ? static_cast<uint16_t>(syncedReport.validPeerCount + 1U)
                                        : syncedReport.validPeerCount;

    const GlobalReadingStats& displayedStats = policy.useAllSynced ? syncedReport.stats : localStats;
    ReadingStatsDateTime now;
    const bool hasCurrentDateTime = getCurrentLocalReadingStatsDateTime(now);
    if (hasCurrentDateTime && localStatsTrusted) {
      bookSummary.hasTodayReadingSeconds = localStats.readingSummaryForDate(
          now.date, bookSummary.todayReadingSeconds, bookSummary.todayReadingSessions);
    }
    if (policy.globalStats != DashboardMetricState::Available) return;
    bookSummary.globalReadingSeconds = displayedStats.totalReadingSeconds;
    bookSummary.globalPagesTurned = displayedStats.totalPagesTurned;
    if (hasCurrentDateTime) {
      bookSummary.currentStreak = displayedStats.currentReadingStreak(&now.date);
      bookSummary.hasCurrentStreak = true;
    }
  };

  // XTC, TXT and other formats still use the recent book's title, author and
  // cover. Their readers do not currently feed the per-book stats engine, so
  // Dashboard labels those metrics as not tracked instead of inventing EPUB
  // progress for them.
  if (!policyInput.isEpub) {
    applyPolicy();
    return;
  }

  // Epub's constructor only derives the cache key. Dashboard verifies the
  // durable source binding once and then opens only the existing book.bin;
  // it never indexes book contents or performs the Reader's second
  // central-directory check during Home startup.
  Epub recentEpub(recentBooks[0].path, "/.crosspoint");

  // Validate the metadata against the backing EPUB before reading any
  // path-keyed statistics or progress. A replaced, legacy, unreadable, or
  // otherwise invalid cache stays unknown on Home; Reader performs any safe
  // rebuild/reset when the user opens the book.
  if (recentEpub.inspectSourceBinding() != Epub::SourceBindingStatus::Match ||
      recentEpub.inspectCache() != BookMetadataCache::LoadStatus::Loaded) {
    applyPolicy();
    bookSummary.progressState = DashboardMetricState::Unavailable;
    return;
  }
  policyInput.epubVerified = true;

  BookReadingStats::LoadStatus statsStatus = BookReadingStats::LoadStatus::Missing;
  const BookReadingStats stats = BookReadingStats::load(recentEpub.getCachePath(), &statsStatus);
  policyInput.bookStatsTrusted = BookReadingStats::isTrustedLoadStatus(statsStatus);
  policyInput.bookStatsMissing = statsStatus == BookReadingStats::LoadStatus::Missing;
  applyPolicy();
  if (bookSummary.bookStatsState == DashboardMetricState::Available) {
    bookSummary.bookReadingSeconds = stats.totalReadingSeconds;
    bookSummary.bookPagesTurned = stats.totalPagesTurned;
    bookSummary.bookSessions = stats.sessionCount;
    bookSummary.hasStartedReading = stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
                                    stats.sessionCount > 0 || stats.isCompleted;
  }

  const auto cacheReadingStatsPresentation = [&](const ReadingStatsMetric progress) {
    // Never turn unreadable/newer files into plausible zeroes. Missing files
    // are trusted, explicit zero-data states.
    if (!policyInput.bookStatsTrusted || !localStatsTrusted) return;
    ReadingStatsDateTime now;
    const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
    readingStatsPresentation =
        buildReadingStatsPresentation(stats, true, localStats, true, syncedReport, currentDateTime, progress, false);
  };

  // Completion is authoritative user state after Epub::load() has verified
  // that this cache still belongs to the EPUB at the recent path. It must not
  // disappear merely because a rebuild cleared the derived section cache or a
  // progress file is unavailable.
  if (DashboardProgress::fromCompletedStats(BookReadingStats::isTrustedLoadStatus(statsStatus), stats.isCompleted,
                                            bookSummary.progressPercent)) {
    bookSummary.hasProgress = true;
    bookSummary.hasStartedReading = true;
    bookSummary.progressState = DashboardMetricState::Available;
    cacheReadingStatsPresentation(ReadingStatsMetric::known(100));
    return;
  }

  std::array<uint8_t, 6> progressBytes{};
  const DashboardProgressValidationContext progressContext{&recentEpub.getCachePath(), recentEpub.getSpineItemsCount()};
  const ProgressFile::CandidateValidator progressValidator{validateDashboardProgressCandidate, &progressContext};
  const ProgressFile::LoadResult progressLoad =
      ProgressFile::loadEpub(recentEpub.getCachePath(), progressBytes.data(), progressBytes.size(), progressValidator);
  // Legacy four-byte progress has no persisted chapter total, so it cannot
  // support an honest Dashboard percentage. A verified six-byte backup/temp is
  // still usable when the canonical file was interrupted or malformed.
  if (!progressLoad || progressLoad.size != progressBytes.size()) {
    bookSummary.progressState = progressLoad.source == ProgressFile::LoadSource::Missing
                                    ? DashboardMetricState::NoData
                                    : DashboardMetricState::Unavailable;
    cacheReadingStatsPresentation(ReadingStatsMetric::unavailable());
    return;
  }

  DashboardProgress::Position progress;
  if (!DashboardProgress::decode(progressBytes.data(), progressBytes.size(), progress)) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    cacheReadingStatsPresentation(ReadingStatsMetric::unavailable());
    return;
  }

  const float chapterProgress = static_cast<float>(progress.pageNumber + 1U) / static_cast<float>(progress.pageCount);
  float bookProgress = 0.0F;
  if (!recentEpub.calculateProgressChecked(progress.spineIndex, chapterProgress, bookProgress) ||
      !DashboardProgress::toPercent(bookProgress, bookSummary.progressPercent)) {
    bookSummary.progressState = DashboardMetricState::Unavailable;
    cacheReadingStatsPresentation(ReadingStatsMetric::unavailable());
    return;
  }
  bookSummary.hasProgress = true;
  bookSummary.progressBelowOnePercent = bookProgress > 0.0F && bookSummary.progressPercent == 0;
  bookSummary.hasStartedReading = bookSummary.hasStartedReading || progress.spineIndex > 0 || progress.pageNumber > 0;
  bookSummary.progressState = DashboardMetricState::Available;
  bookSummary.hasChapterPage = true;
  bookSummary.chapterPageCurrent = static_cast<uint16_t>(progress.pageNumber + 1U);
  bookSummary.chapterPageTotal = progress.pageCount;
  const int tocIndex = recentEpub.getTocIndexForSpineIndex(progress.spineIndex);
  if (tocIndex >= 0) bookSummary.chapterTitle = recentEpub.getTocItem(tocIndex).title;
  cacheReadingStatsPresentation(ReadingStatsMetric::known(bookSummary.progressPercent));
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      const bool thumbnailExists = Storage.exists(coverPath.c_str());
      const bool validateThumbnail = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::DASHBOARD ||
                                     SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI;
      const bool thumbnailUsable = thumbnailExists && (!validateThumbnail || hasUsableBitmapThumbnail(coverPath));
      if (thumbnailExists && !thumbnailUsable && !Storage.remove(coverPath.c_str())) {
        LOG_ERR("HOME", "Could not remove invalid cover thumbnail: %s", coverPath.c_str());
      }
      if (!thumbnailUsable) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Thumbnail decoding is one of Home's largest transient allocations.
          // Drop the reusable cover snapshot first so both do not coexist.
          freeCoverBuffer();
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          freeCoverBuffer();
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  if (ReadingStatsCompletionTransaction::recoverPending() ==
      ReadingStatsCompletionTransaction::RecoveryResult::Blocked) {
    LOG_ERR("HOME", "Pending reading-statistics transaction remains blocked");
  }

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  // Keep the feature isolated: existing home themes must not pay any stats or
  // synced-snapshot SD I/O. CrossVi additionally shows verified TXT/Markdown
  // and XTC/XTCH summaries without changing their persistence formats.
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::DASHBOARD ||
      SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI) {
    const bool isCrossViNonEpub =
        SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI && !recentBooks.empty() &&
        (FsHelpers::hasXtcExtension(recentBooks.front().path) ||
         FsHelpers::hasTxtExtension(recentBooks.front().path) ||
         FsHelpers::hasMarkdownExtension(recentBooks.front().path));
    bookSummary = {};
    bookSummary.dailyReadingGoalMinutes = SETTINGS.dailyReadingGoalMinutes;
    readingStatsPresentation.reset();
    if (!isCrossViNonEpub || !loadRecentNonEpubReadingStats()) loadBookSummary();
  } else {
    bookSummary = {};
    readingStatsPresentation.reset();
  }

  selectorIndex = initialMenuItem == HomeMenuItem::NONE
                      ? 0
                      : HomeMenuMapping::selectorIndexOf(initialMenuItem, static_cast<int>(recentBooks.size()),
                                                         hasOpdsServers, hasReadingStatsShortcut());
  if (selectorIndex < 0) {
    // Preserve the previous fallback for an optional destination that vanished
    // while returning Home (for example, the last OPDS server was removed).
    selectorIndex = HomeMenuMapping::selectorIndexOf(HomeMenuItem::FILE_BROWSER, static_cast<int>(recentBooks.size()),
                                                     hasOpdsServers, hasReadingStatsShortcut());
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (readingStatsNotice != ReadingStatsNotice::None) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      readingStatsNotice = ReadingStatsNotice::None;
      requestUpdate();
    }
    return;
  }

  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex, hasOpdsServers, hasReadingStatsShortcut())) {
        case HomeMenuItem::FILE_BROWSER:
          onFileBrowserOpen();
          break;
        case HomeMenuItem::RECENTS:
          onRecentsOpen();
          break;
        case HomeMenuItem::SAVED_ITEMS:
          onSavedItemsOpen();
          break;
        case HomeMenuItem::OPDS_BROWSER:
          onOpdsBrowserOpen();
          break;
        case HomeMenuItem::FILE_TRANSFER:
          onFileTransferOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        case HomeMenuItem::READING_STATS:
          onReadingStatsOpen();
          break;
        default:
          break;
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHomeHeader(
      renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
      metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  const Rect homeTile{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight};
  const Rect cacheRect = GUI.getHomeCoverCacheRect(homeTile);
  coverRectX = cacheRect.x;
  coverRectY = cacheRect.y;
  coverRectW = cacheRect.width;
  coverRectH = cacheRect.height;

  GUI.drawHomeContent(renderer, homeTile, recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                      std::bind(&HomeActivity::storeCoverBuffer, this), bookSummary);

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_CLIPPINGS),
                                        tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Bookmark, Transfer, Settings};

  if (hasReadingStatsShortcut()) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_READING_STATS));
    menuIcons.insert(menuIcons.begin() + 2, Book);
  }

  if (hasOpdsServers) {
    const size_t opdsIndex = 3 + (hasReadingStatsShortcut() ? 1u : 0u);
    menuItems.insert(menuItems.begin() + static_cast<std::ptrdiff_t>(opdsIndex), tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + static_cast<std::ptrdiff_t>(opdsIndex), Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const bool showReadingSummary = hasHomeReadingSummary();
  const int summaryY = contentBottom - CrossViMetrics::HOME_READING_SUMMARY_HEIGHT -
                       CrossViMetrics::HOME_READING_SUMMARY_BOTTOM_GAP;
  const int menuBottom = showReadingSummary ? summaryY - CrossViMetrics::HOME_READING_SUMMARY_GAP : contentBottom;
  GUI.drawButtonMenu(
      renderer, Rect{0, menuTop, pageWidth, std::max(0, menuBottom - menuTop)}, static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  if (showReadingSummary) {
    GUI.drawHomeReadingSummary(renderer,
                               Rect{CrossViMetrics::values.contentSidePadding, summaryY,
                                    pageWidth - CrossViMetrics::values.contentSidePadding * 2,
                                    CrossViMetrics::HOME_READING_SUMMARY_HEIGHT},
                               bookSummary, false);
  }

  const char* resumeLabel = recentBooks.empty() ? "" : tr(STR_RESUME);
  if (!recentBooks.empty() && SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CROSSVI) {
    resumeLabel = I18N.get(homeBookHintLabelId(bookSummary));
  }
  const auto labels =
      mappedInput.mapLabels(resumeLabel, tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  switch (readingStatsNotice) {
    case ReadingStatsNotice::BackupDone:
      GUI.drawPopup(renderer, tr(STR_STATS_BACKUP_DONE));
      break;
    case ReadingStatsNotice::BackupFailed:
      GUI.drawPopup(renderer, tr(STR_STATS_BACKUP_FAILED));
      break;
    case ReadingStatsNotice::RestoreDone:
      GUI.drawPopup(renderer, tr(STR_STATS_RESTORE_DONE));
      break;
    case ReadingStatsNotice::RestoreFailed:
      GUI.drawPopup(renderer, tr(STR_STATS_RESTORE_FAILED));
      break;
    case ReadingStatsNotice::NoBackup:
      GUI.drawPopup(renderer, tr(STR_STATS_NO_BACKUP));
      break;
    case ReadingStatsNotice::None:
      break;
  }

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSavedItemsOpen() { activityManager.goToSavedClippings(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

bool HomeActivity::loadRecentNonEpubReadingStats() {
  if (recentBooks.empty()) return false;
  const std::string& path = recentBooks.front().path;
  std::string cachePath;
  ZipFile::SourceIdentity currentIdentity;
  ZipFile::SourceIdentity storedIdentity;
  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  const bool isXtc = FsHelpers::hasXtcExtension(path);
  uint32_t xtcPageCount = 0;
  size_t txtFileSize = 0;

  if (isXtc) {
    Xtc xtc(path, "/.crosspoint");
    if (!xtc.load() || !xtc.getSourceIdentity(currentIdentity)) return false;
    cachePath = xtc.getCachePath();
    xtcPageCount = xtc.getPageCount();
    if (xtcPageCount == 0) return false;
  } else {
    Txt txt(path, "/.crosspoint");
    if (!txt.load() || !txt.getSourceIdentity(currentIdentity)) return false;
    cachePath = txt.getCachePath();
    txtFileSize = txt.getFileSize();
  }

  const SourceIdentityStore::LoadStatus identityStatus = SourceIdentityStore::load(cachePath, storedIdentity);
  const bool identityTrusted = identityStatus == SourceIdentityStore::LoadStatus::Primary ||
                               identityStatus == SourceIdentityStore::LoadStatus::Backup ||
                               identityStatus == SourceIdentityStore::LoadStatus::Temp;
  if (!identityTrusted || storedIdentity != currentIdentity) return false;

  if (isXtc) {
    uint8_t progressBytes[4]{};
    const ProgressFile::PageBounds bounds{xtcPageCount};
    const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
    const ProgressFile::LoadResult loaded =
        ProgressFile::loadPage(cachePath, progressBytes, sizeof(progressBytes), validator);
    if (loaded) {
      const uint32_t page = ProgressFileCodec::decodePage(progressBytes);
      const uint32_t percent = static_cast<uint32_t>(
          std::min<uint64_t>(100, (static_cast<uint64_t>(page) + 1u) * 100u / xtcPageCount));
      progress = ReadingStatsMetric::known(percent);
      bookSummary.hasProgress = true;
      bookSummary.progressBelowOnePercent = percent == 0;
      bookSummary.hasStartedReading = page > 0;
      bookSummary.progressPercent = static_cast<uint8_t>(percent);
      bookSummary.progressState = DashboardMetricState::Available;
    } else {
      bookSummary.progressState = loaded.source == ProgressFile::LoadSource::Missing
                                      ? DashboardMetricState::NoData
                                      : DashboardMetricState::Unavailable;
    }
  } else if (txtFileSize > 0) {
    uint8_t progressBytes[ProgressFileCodec::TXT_V2_SIZE]{};
    const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(txtFileSize), 0};
    const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
    const ProgressFile::LoadResult loaded =
        ProgressFile::loadTxt(cachePath, progressBytes, sizeof(progressBytes), validator);
    uint32_t byteOffset = 0;
    if (loaded && ProgressFileCodec::decodeTxt(progressBytes, loaded.size, byteOffset) ==
                      ProgressFileCodec::TxtDecodeStatus::Ok) {
      const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(byteOffset) * 100u / txtFileSize);
      progress = ReadingStatsMetric::estimated(percent);
      bookSummary.hasProgress = true;
      bookSummary.hasStartedReading = byteOffset > 0;
      bookSummary.progressEstimated = true;
      bookSummary.progressPercent = static_cast<uint8_t>(std::min<uint32_t>(percent, 100u));
      bookSummary.progressState = DashboardMetricState::Available;
    } else {
      bookSummary.progressState = loaded.source == ProgressFile::LoadSource::Missing
                                      ? DashboardMetricState::NoData
                                      : DashboardMetricState::Unavailable;
    }
  } else {
    bookSummary.progressState = DashboardMetricState::NoData;
  }

  BookReadingStats::LoadStatus bookStatus = BookReadingStats::LoadStatus::Missing;
  const BookReadingStats bookStats = BookReadingStats::load(cachePath, &bookStatus);
  GlobalReadingStats::LoadStatus deviceStatus = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats deviceStats = GlobalReadingStats::load(&deviceStatus);
  if (!BookReadingStats::isTrustedLoadStatus(bookStatus) || !GlobalReadingStats::isTrustedLoadStatus(deviceStatus)) {
    return false;
  }

  bookSummary.bookStatsState = bookStatus == BookReadingStats::LoadStatus::Missing ? DashboardMetricState::NoData
                                                                                   : DashboardMetricState::Available;
  bookSummary.bookReadingSeconds = bookStats.totalReadingSeconds;
  bookSummary.bookPagesTurned = bookStats.totalPagesTurned;
  bookSummary.bookSessions = bookStats.sessionCount;
  bookSummary.hasStartedReading = bookSummary.hasStartedReading || bookStats.totalReadingSeconds > 0 ||
                                  bookStats.totalPagesTurned > 0 || bookStats.sessionCount > 0 ||
                                  bookStats.isCompleted;

  if (bookStats.isCompleted) {
    progress = ReadingStatsMetric::known(100);
    bookSummary.hasProgress = true;
    bookSummary.hasStartedReading = true;
    bookSummary.progressEstimated = false;
    bookSummary.progressPercent = 100;
    bookSummary.progressState = DashboardMetricState::Available;
  }

  const bool hasSyncedDirectory = GlobalReadingStats::hasSyncedStats();
  const GlobalReadingStatsAggregation allSyncedStats =
      hasSyncedDirectory ? GlobalReadingStats::loadAggregatedWithReport(deviceStats) : GlobalReadingStatsAggregation{};
  DashboardStatsPolicyInput policyInput;
  policyInput.localStatsTrusted = true;
  policyInput.localStatsMissing = deviceStatus == GlobalReadingStats::LoadStatus::Missing;
  policyInput.hasSyncedDirectory = hasSyncedDirectory;
  policyInput.syncedScanComplete = allSyncedStats.scanComplete;
  policyInput.validPeerCount = allSyncedStats.validPeerCount;
  policyInput.skippedPeerCount = allSyncedStats.skippedPeerCount;
  const DashboardStatsPolicyResult policy = DashboardStatsPolicy::evaluate(policyInput);
  bookSummary.globalStatsState = policy.globalStats;
  bookSummary.syncedStatsState = policy.syncedStats;
  bookSummary.usingSyncedStats = policy.useAllSynced;
  bookSummary.syncedDeviceCount = policy.useAllSynced && allSyncedStats.validPeerCount != UINT16_MAX
                                      ? static_cast<uint16_t>(allSyncedStats.validPeerCount + 1U)
                                      : allSyncedStats.validPeerCount;
  const GlobalReadingStats& displayedStats = policy.useAllSynced ? allSyncedStats.stats : deviceStats;
  if (policy.globalStats == DashboardMetricState::Available) {
    bookSummary.globalReadingSeconds = displayedStats.totalReadingSeconds;
    bookSummary.globalPagesTurned = displayedStats.totalPagesTurned;
  }
  ReadingStatsDateTime now;
  const ReadingStatsDateTime* currentDateTime = getCurrentLocalReadingStatsDateTime(now) ? &now : nullptr;
  if (currentDateTime && policy.globalStats == DashboardMetricState::Available) {
    bookSummary.currentStreak = displayedStats.currentReadingStreak(&now.date);
    bookSummary.hasCurrentStreak = true;
  }
  if (currentDateTime) {
    bookSummary.hasTodayReadingSeconds = deviceStats.readingSummaryForDate(
        now.date, bookSummary.todayReadingSeconds, bookSummary.todayReadingSessions);
  }
  readingStatsPresentation = buildReadingStatsPresentation(bookStats, true, deviceStats, true, allSyncedStats,
                                                           currentDateTime, progress, false);
  markReadingStatsPageMetricsNotApplicable(*readingStatsPresentation);
  return true;
}

void HomeActivity::onReadingStatsOpen() {
  if (!hasReadingStatsShortcut() && !hasHomeReadingSummary()) return;
  if (!readingStatsPresentation) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    renderer.displayBuffer();
    if (recentBooks.empty() || FsHelpers::hasEpubExtension(recentBooks.front().path)) {
      loadBookSummary();
    } else if (!loadRecentNonEpubReadingStats()) {
      requestUpdate();
      return;
    }
  }
  if (!readingStatsPresentation) {
    requestUpdate();
    return;
  }
  const std::string statsTitle = recentBooks.empty() ? std::string(tr(STR_READING_STATS)) : recentBooks.front().title;
  startActivityForResult(std::make_unique<ReadingStatsActivity>(
                             renderer, mappedInput, statsTitle, *readingStatsPresentation,
                             ReadingStatsActivity::Page::Device, false, true),
                         [this](const ActivityResult& result) {
                           const auto* action = std::get_if<ReadingStatsActionResult>(&result.data);
                           if (!action) {
                             requestUpdate();
                             return;
                           }
                           if (action->action == ReadingStatsActionResult::Action::BackupDeviceStats) {
                             const GlobalReadingStats::BackupResult backup = GlobalReadingStats::createBackup();
                             readingStatsNotice = backup == GlobalReadingStats::BackupResult::Ok
                                                      ? ReadingStatsNotice::BackupDone
                                                      : ReadingStatsNotice::BackupFailed;
                             requestUpdate();
                             return;
                           }
                           if (action->action != ReadingStatsActionResult::Action::RestoreDeviceStats) return;
                           startActivityForResult(
                               std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_STATS_RESTORE),
                                                                      tr(STR_STATS_RESTORE_PROMPT)),
                               [this](const ActivityResult& confirmation) {
                                 if (confirmation.isCancelled) return;
                                 const GlobalReadingStats::BackupResult restored =
                                     GlobalReadingStats::restoreBackup();
                                 if (restored == GlobalReadingStats::BackupResult::Ok) {
                                   readingStatsPresentation.reset();
                                   if (recentBooks.empty() || FsHelpers::hasEpubExtension(recentBooks.front().path)) {
                                     loadBookSummary();
                                   } else {
                                     loadRecentNonEpubReadingStats();
                                   }
                                   readingStatsNotice = ReadingStatsNotice::RestoreDone;
                                 } else if (restored == GlobalReadingStats::BackupResult::Missing) {
                                   readingStatsNotice = ReadingStatsNotice::NoBackup;
                                 } else {
                                   readingStatsNotice = ReadingStatsNotice::RestoreFailed;
                                 }
                                 requestUpdate();
                               });
                         });
}
