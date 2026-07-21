#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
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
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ReadingStatsCompletionTransaction.h"
#include "components/UITheme.h"
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
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // File Browser, Recents, Saved Items, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
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

  const GlobalReadingStats localStats = GlobalReadingStats::load();
  const GlobalReadingStats globalStats =
      GlobalReadingStats::hasSyncedStats() ? GlobalReadingStats::loadAggregated(localStats) : localStats;
  bookSummary.globalReadingSeconds = globalStats.totalReadingSeconds;
  bookSummary.globalPagesTurned = globalStats.totalPagesTurned;
  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    bookSummary.currentStreak = globalStats.currentReadingStreak(&now.date);
  }

  if (recentBooks.empty() || !FsHelpers::hasEpubExtension(recentBooks[0].path)) return;

  // Epub's constructor only derives the cache key; it does not open or parse
  // the book, so Dashboard never indexes book contents during Home startup.
  Epub recentEpub(recentBooks[0].path, "/.crosspoint");

  // Validate the metadata against the backing EPUB before reading any
  // path-keyed statistics or progress. A replaced, legacy, unreadable, or
  // otherwise invalid cache stays unknown on Home; Reader performs any safe
  // rebuild/reset when the user opens the book.
  if (!recentEpub.load(false, true)) return;

  BookReadingStats::LoadStatus statsStatus = BookReadingStats::LoadStatus::Missing;
  const BookReadingStats stats = BookReadingStats::load(recentEpub.getCachePath(), &statsStatus);
  bookSummary.bookReadingSeconds = stats.totalReadingSeconds;
  bookSummary.bookPagesTurned = stats.totalPagesTurned;
  bookSummary.bookSessions = stats.sessionCount;
  bookSummary.estimatedTimeLeftSeconds = stats.estimatedTimeLeftSeconds;

  // Completion is authoritative user state after Epub::load() has verified
  // that this cache still belongs to the EPUB at the recent path. It must not
  // disappear merely because a rebuild cleared the derived section cache or a
  // progress file is unavailable.
  if (DashboardProgress::fromCompletedStats(BookReadingStats::isTrustedLoadStatus(statsStatus), stats.isCompleted,
                                            bookSummary.progressPercent)) {
    bookSummary.hasProgress = true;
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
  if (!progressLoad || progressLoad.size != progressBytes.size()) return;

  DashboardProgress::Position progress;
  if (!DashboardProgress::decode(progressBytes.data(), progressBytes.size(), progress)) return;

  const float chapterProgress = static_cast<float>(progress.pageNumber + 1U) / static_cast<float>(progress.pageCount);
  if (!DashboardProgress::toPercent(recentEpub.calculateProgress(progress.spineIndex, chapterProgress),
                                    bookSummary.progressPercent)) {
    return;
  }
  bookSummary.hasProgress = true;
  const int tocIndex = recentEpub.getTocIndexForSpineIndex(progress.spineIndex);
  if (tocIndex >= 0) bookSummary.chapterTitle = recentEpub.getTocItem(tocIndex).title;
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
      const bool validateThumbnail = SETTINGS.uiTheme == CrossPointSettings::UI_THEME::DASHBOARD;
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
  // synced-snapshot SD I/O when Dashboard is not selected.
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::DASHBOARD) {
    loadBookSummary();
  } else {
    bookSummary = {};
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

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
      switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
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
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_SAVED_ITEMS),
                                        tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Bookmark, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 3, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 3, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

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
