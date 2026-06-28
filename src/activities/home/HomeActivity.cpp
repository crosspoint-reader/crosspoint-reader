#include "HomeActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <vector>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"

void HomeActivity::buildHomeActions(std::vector<ThemeHomeActionEntry>& actions) const {
  buildThemeHomeActions(UITheme::getInstance().getHomeScreenSpec(), recentBooks, hasOpdsServers, actions);
}

const std::vector<ThemeHomeActionEntry>& HomeActivity::refreshHomeActions() {
  buildHomeActions(homeActions);
  return homeActions;
}

int HomeActivity::getMenuItemCount() { return static_cast<int>(refreshHomeActions().size()); }

bool HomeActivity::storeCoverBufferCallback(void* userData) {
  auto* activity = static_cast<HomeActivity*>(userData);
  return activity != nullptr && activity->storeCoverBuffer();
}

bool HomeActivity::restoreCoverBufferCallback(void* userData) {
  auto* activity = static_cast<HomeActivity*>(userData);
  return activity != nullptr && activity->restoreCoverBuffer();
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

void HomeActivity::loadRecentCovers(const std::vector<int>& coverHeights) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      bool hasMissingThumb = false;
      for (const int coverHeight : coverHeights) {
        std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (!Storage.exists(coverPath.c_str())) {
          hasMissingThumb = true;
          break;
        }
      }

      if (hasMissingThumb) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = true;
          for (const int coverHeight : coverHeights) {
            std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
            if (!Storage.exists(coverPath.c_str())) {
              success = epub.generateThumbBmp(coverHeight) && success;
            }
          }
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            for (const int coverHeight : coverHeights) {
              std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
              if (!Storage.exists(coverPath.c_str())) {
                success = xtc.generateThumbBmp(coverHeight) && success;
              }
            }
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

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  LOG_DBG("HOME", "Loaded %d/%d recent book(s) for home theme", static_cast<int>(recentBooks.size()),
          metrics.homeRecentBooksCount);

  const auto& actions = refreshHomeActions();
  const ThemeHomeScreenSpec* homeSpec = UITheme::getInstance().getHomeScreenSpec();
  selectorIndex = 0;
  bool hasWantedAction = initialMenuItem != HomeMenuItem::NONE;
  const auto wantedAction = [this]() {
    switch (initialMenuItem) {
      case HomeMenuItem::RECENTS:
        return ThemeHomeAction::RecentBooks;
      case HomeMenuItem::OPDS_BROWSER:
        return ThemeHomeAction::OpdsBrowser;
      case HomeMenuItem::FILE_TRANSFER:
        return ThemeHomeAction::FileTransfer;
      case HomeMenuItem::SETTINGS_MENU:
        return ThemeHomeAction::Settings;
      case HomeMenuItem::FILE_BROWSER:
      case HomeMenuItem::NONE:
      default:
        return ThemeHomeAction::FileBrowser;
    }
  }();
  ThemeHomeAction selectedEntryAction = wantedAction;
  if (!hasWantedAction && homeSpec != nullptr && homeSpec->hasInitialAction) {
    hasWantedAction = true;
    selectedEntryAction = homeSpec->initialAction;
  }
  if (hasWantedAction) {
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
      if (actions[i].action == selectedEntryAction) {
        selectorIndex = i;
        break;
      }
    }
  }
  coverSelectorIndex = !recentBooks.empty() && selectorIndex < static_cast<int>(actions.size()) &&
                               actions[selectorIndex].action == ThemeHomeAction::RecentBook
                           ? actions[selectorIndex].value
                           : 0;

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
  coverBuffer = makeUniqueNoThrow<uint8_t[]>(needed);
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                   coverBufferSize)) {
    coverBuffer.reset();
    coverBufferSize = 0;
    return false;
  }
  coverBufferSelectorIndex = coverSelectorIndex;
  const auto& actions = refreshHomeActions();
  coverBufferStripSelected = selectorIndex >= 0 && selectorIndex < static_cast<int>(actions.size()) &&
                             actions[selectorIndex].action == ThemeHomeAction::RecentBook;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                     coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  coverBuffer.reset();
  coverBufferSize = 0;
  coverBufferStored = false;
  coverBufferSelectorIndex = -1;
  coverBufferStripSelected = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const ThemeHomeScreenSpec* homeSpec = UITheme::getInstance().getHomeScreenSpec();

  auto updateCoverSelection = [this]() {
    const auto& actions = refreshHomeActions();
    if (selectorIndex < static_cast<int>(actions.size()) &&
        actions[selectorIndex].action == ThemeHomeAction::RecentBook) {
      coverSelectorIndex = actions[selectorIndex].value;
    }
  };

  auto moveWithin = [this, &updateCoverSelection](bool wantRecentBook, int delta) {
    const auto& actions = refreshHomeActions();
    if (actions.empty()) return;

    navigationIndices.clear();
    navigationIndices.reserve(actions.size());
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
      if ((actions[i].action == ThemeHomeAction::RecentBook) == wantRecentBook) {
        navigationIndices.push_back(i);
      }
    }
    if (navigationIndices.empty()) return;

    auto current = std::find(navigationIndices.begin(), navigationIndices.end(), selectorIndex);
    int groupIndex = current == navigationIndices.end() ? (delta > 0 ? -1 : 0)
                                                        : static_cast<int>(current - navigationIndices.begin());
    groupIndex =
        (groupIndex + delta + static_cast<int>(navigationIndices.size())) % static_cast<int>(navigationIndices.size());
    selectorIndex = navigationIndices[groupIndex];
    updateCoverSelection();
    requestUpdate();
  };

  if (homeSpec != nullptr && homeSpec->navigation == ThemeHomeNavigationMode::SplitAxis) {
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [moveWithin] { moveWithin(false, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [moveWithin] { moveWithin(false, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [moveWithin] { moveWithin(true, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [moveWithin] { moveWithin(true, -1); });
  } else if (homeSpec != nullptr && homeSpec->navigation == ThemeHomeNavigationMode::CarouselAxis) {
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [moveWithin] { moveWithin(true, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [moveWithin] { moveWithin(true, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [moveWithin] { moveWithin(false, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [moveWithin] { moveWithin(false, -1); });
  } else {
    buttonNavigator.onNext([this, menuCount, &updateCoverSelection] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      updateCoverSelection();
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount, &updateCoverSelection] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      updateCoverSelection();
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& actions = refreshHomeActions();
    if (selectorIndex < 0 || selectorIndex >= static_cast<int>(actions.size())) return;
    const auto& entry = actions[selectorIndex];
    switch (entry.action) {
      case ThemeHomeAction::RecentBook:
        if (entry.value >= 0 && entry.value < static_cast<int>(recentBooks.size()))
          onSelectBook(recentBooks[entry.value].path);
        break;
      case ThemeHomeAction::RecentBooks:
        onRecentsOpen();
        break;
      case ThemeHomeAction::OpdsBrowser:
        onOpdsBrowserOpen();
        break;
      case ThemeHomeAction::FileTransfer:
        onFileTransferOpen();
        break;
      case ThemeHomeAction::Settings:
        onSettingsOpen();
        break;
      case ThemeHomeAction::FileBrowser:
      default:
        onFileBrowserOpen();
        break;
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int coverCacheBleed = 12;
  const ThemeHomeScreenSpec* homeSpec = UITheme::getInstance().getHomeScreenSpec();

  if (homeSpec != nullptr) {
    const auto& actions = refreshHomeActions();
    ThemeHomeRenderContext context{renderer,
                                   mappedInput,
                                   metrics,
                                   *homeSpec,
                                   recentBooks,
                                   actions,
                                   hasOpdsServers,
                                   selectorIndex,
                                   coverSelectorIndex,
                                   coverRendered,
                                   coverBufferStored,
                                   coverBufferSelectorIndex,
                                   coverBufferStripSelected,
                                   coverRectX,
                                   coverRectY,
                                   coverRectW,
                                   coverRectH,
                                   this,
                                   &HomeActivity::storeCoverBufferCallback,
                                   &HomeActivity::restoreCoverBufferCallback};
    if (renderThemeHome(context)) {
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading && !UITheme::getInstance().getHomeCoverThumbHeights().empty()) {
        recentsLoading = true;
        loadRecentCovers(UITheme::getInstance().getHomeCoverThumbHeights());
      }
      return;
    }
  }

  const bool hasCoverArea = metrics.homeCoverTileHeight > 0 && metrics.homeCoverHeight > 0;

  renderer.clearScreen();

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. Include a small bleed
  // because cover-strip themes can draw selection outlines just outside the
  // nominal cover tile.
  coverRectX = 0;
  coverRectY = hasCoverArea ? std::max(0, metrics.homeTopPadding - coverCacheBleed) : 0;
  coverRectW = pageWidth;
  coverRectH = hasCoverArea
                   ? std::min(pageHeight - coverRectY,
                              metrics.homeCoverTileHeight + (metrics.homeTopPadding - coverRectY) + coverCacheBleed)
                   : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && metrics.homeShowContinueReadingHeader && !recentBooks.empty()
                     ? recentBooks[std::min(coverSelectorIndex, static_cast<int>(recentBooks.size()) - 1)].title.c_str()
                     : nullptr);

  const bool selectorSensitiveCoverCache = GUI.homeCoverCacheDependsOnSelector();
  const bool coverStripSelected = metrics.homeContinueReadingInMenu
                                      ? selectorIndex == 0 && !recentBooks.empty()
                                      : selectorIndex < static_cast<int>(recentBooks.size());
  const bool coverCacheMatches = !selectorSensitiveCoverCache || (coverBufferSelectorIndex == coverSelectorIndex &&
                                                                  coverBufferStripSelected == coverStripSelected);
  if (hasCoverArea && coverBufferStored && !coverCacheMatches) {
    freeCoverBuffer();
    coverRendered = false;
  }
  bool bufferRestored = hasCoverArea && coverBufferStored && coverCacheMatches && restoreCoverBuffer();

  if (hasCoverArea) {
    GUI.drawRecentBookCover(
        renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks,
        coverSelectorIndex, coverRendered, coverBufferStored, bufferRestored, [this]() { return storeCoverBuffer(); },
        coverStripSelected);
  } else {
    coverRendered = false;
    coverBufferStored = false;
    bufferRestored = false;
  }

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
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

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading && !UITheme::getInstance().getHomeCoverThumbHeights().empty()) {
    recentsLoading = true;
    loadRecentCovers(UITheme::getInstance().getHomeCoverThumbHeights());
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
