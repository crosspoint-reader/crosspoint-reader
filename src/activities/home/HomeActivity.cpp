#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;
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
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  Rect popupRect;

  static constexpr uint32_t COVER_RENDER_TIMEOUT_MS = 10000;
  const bool isForcedBook = !recentBooks.empty() && (recentBooks[0].path == APP_STATE.forceRenderCoverPath);
  LOG_DBG("HOME", "loadRecentCovers: coverMode=%d isForced=%d forcePath='%s'", SETTINGS.coverMode, isForcedBook,
          APP_STATE.forceRenderCoverPath.c_str());
  if (SETTINGS.coverMode == CrossPointSettings::COVER_DISABLED_MODE && !isForcedBook) {
    LOG_DBG("HOME", "loadRecentCovers: skipped (globally disabled)");
    APP_STATE.forceRenderCoverPath = "";
    recentsLoaded = true;
    recentsLoading = false;
    return;
  }
  const bool useTimeout = (SETTINGS.coverMode == CrossPointSettings::COVER_TIMEOUT);
  LOG_DBG("HOME", "loadRecentCovers: useTimeout=%d timeoutMs=%u", useTimeout, COVER_RENDER_TIMEOUT_MS);
  if (!recentBooks.empty()) {
    RecentBook& book = recentBooks[0];
    LOG_DBG("HOME", "loadRecentCovers: book='%s' coverBmp='%s' coverDisabled=%d", book.path.c_str(),
            book.coverBmpPath.c_str(), book.coverDisabled);
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      bool coverExists = Storage.exists(coverPath.c_str());
      LOG_DBG("HOME", "loadRecentCovers: coverPath='%s' exists=%d isForced=%d", coverPath.c_str(), coverExists,
              isForcedBook);
      if ((isForcedBook || !coverExists) && (!book.coverDisabled || isForcedBook)) {
        if (FsHelpers::hasEpubExtension(book.path)) {
          if (isForcedBook) {
            Storage.remove(coverPath.c_str());
            LOG_DBG("HOME", "loadRecentCovers: force-render removed stale BMP");
          }
          Epub epub(book.path, "/.crosspoint");
          epub.load(false, true);
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          GUI.fillPopupProgress(renderer, popupRect, 50);
          const uint32_t deadline = (useTimeout && !isForcedBook) ? (millis() + COVER_RENDER_TIMEOUT_MS) : 0;
          LOG_DBG("HOME", "loadRecentCovers: generating cover (deadline=%u ms)", deadline);
          bool success = epub.generateThumbBmp(coverHeight, deadline);
          LOG_DBG("HOME", "loadRecentCovers: generateThumbBmp result=%d", success);
          if (success) {
            RECENT_BOOKS.setCoverDisabled(book.path, false);
            book.coverDisabled = false;
          } else {
            RECENT_BOOKS.setCoverDisabled(book.path, true);
            book.coverDisabled = true;
          }
          coverRendered = false;
          coverBufferStored = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            GUI.fillPopupProgress(renderer, popupRect, 50);
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        }
      }
    }
  }

  APP_STATE.forceRenderCoverPath = "";
  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  coverRendered = false;
  coverBufferStored = false;
  LOG_DBG("HOME", "onEnter: resetting coverRendered=%d coverBufferStored=%d coverBuffer=%p", coverRendered,
          coverBufferStored, static_cast<const void*>(coverBuffer));
  selectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  LOG_DBG("HOME", "onExit: coverRendered=%d coverBufferStored=%d", coverRendered, coverBufferStored);
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  LOG_DBG("HOME", "storeCoverBuffer: stored %zu bytes", bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  LOG_DBG("HOME", "restoreCoverBuffer: restored %zu bytes", bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsServers ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  LOG_DBG("HOME", "render: bufferRestored=%d coverRendered=%d coverBufferStored=%d", bufferRestored, coverRendered,
          coverBufferStored);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu) {
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
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoaded = true;
    const bool hasPending = APP_STATE.pendingCoverGeneration;
    const bool hasForce = !APP_STATE.forceRenderCoverPath.empty();
    LOG_DBG("HOME", "render: checking covers generation: hasPending=%d hasForce=%d", hasPending, hasForce);
    if (hasPending || hasForce) {
      APP_STATE.pendingCoverGeneration = false;
      recentsLoaded = false;
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
  }
}

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
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

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
