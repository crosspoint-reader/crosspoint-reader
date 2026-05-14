#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width, int height,
                                const std::string& fileName) {
  Storage.mkdir(cacheDir.c_str());
  const std::string bmpPath = cacheDir + "/" + fileName;
  if (Storage.exists(bmpPath.c_str())) return bmpPath;

  FsFile src;
  if (!Storage.openFileForRead("HOME", sidecarPath, src)) return "";
  FsFile dst;
  if (!Storage.openFileForWrite("HOME", bmpPath, dst)) {
    src.close();
    return "";
  }

  bool ok = false;
  if (FsHelpers::hasJpgExtension(sidecarPath)) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);
  } else if (FsHelpers::hasPngExtension(sidecarPath)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height);
  }
  src.close();
  dst.close();
  if (!ok) {
    Storage.remove(bmpPath.c_str());
    return "";
  }
  return bmpPath;
}

constexpr int CLASSIC_MIN_RECENT_TILE_HEIGHT = 280;
constexpr int LYRA_MIN_RECENT_TILE_HEIGHT = 170;
constexpr int LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT = 200;
constexpr int CLASSIC_MIN_RECENT_TO_MENU_GAP = 2;
constexpr int LYRA_MIN_RECENT_TO_MENU_GAP = 4;

struct HomeScreenLayout {
  int recentTileHeight;
  int recentToMenuGap;
  int menuHeight;
};

bool isLyraFamilyTheme() {
  const auto theme = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  return theme == CrossPointSettings::UI_THEME::LYRA || theme == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

bool isLyraExtendedTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

int getMinRecentTileHeight() {
  if (isLyraExtendedTheme()) {
    return LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT;
  }
  if (isLyraFamilyTheme()) {
    return LYRA_MIN_RECENT_TILE_HEIGHT;
  }
  return CLASSIC_MIN_RECENT_TILE_HEIGHT;
}

int getMinRecentToMenuGap() {
  return isLyraFamilyTheme() ? LYRA_MIN_RECENT_TO_MENU_GAP : CLASSIC_MIN_RECENT_TO_MENU_GAP;
}

HomeScreenLayout computeHomeScreenLayout(const ThemeMetrics& metrics, int contentHeight, int menuItemCount) {
  HomeScreenLayout layout{metrics.homeCoverTileHeight, metrics.verticalSpacing, 0};

  const int menuRequiredHeight =
      menuItemCount * metrics.menuRowHeight + std::max(0, menuItemCount - 1) * metrics.menuSpacing;

  auto computeMenuHeight = [&]() {
    return contentHeight - (metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap);
  };

  layout.menuHeight = computeMenuHeight();
  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int gapReduction =
      std::min(layout.recentToMenuGap - getMinRecentToMenuGap(), menuRequiredHeight - layout.menuHeight);
  if (gapReduction > 0) {
    layout.recentToMenuGap -= gapReduction;
    layout.menuHeight = computeMenuHeight();
  }

  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int tileReduction =
      std::min(layout.recentTileHeight - getMinRecentTileHeight(), menuRequiredHeight - layout.menuHeight);
  if (tileReduction > 0) {
    layout.recentTileHeight -= tileReduction;
    layout.menuHeight = computeMenuHeight();
  }

  layout.menuHeight = std::max(0, layout.menuHeight);
  return layout;
}

int getHomeCoverRenderHeight(const HomeScreenLayout& layout) {
  return isLyraExtendedTheme() ? std::max(120, layout.recentTileHeight - 58)
                               : std::max(120, layout.recentTileHeight - (isLyraFamilyTheme() ? 16 : 0));
}
}  // namespace

// Builds the menu entry list in display order. Single source of truth for both loop() (which
// dispatches Confirm based on action) and render() (which draws labels/icons).
void HomeActivity::rebuildMenuEntries() {
  menuEntries.clear();
  menuEntries.reserve(7);

  menuEntries.push_back({MenuAction::FileBrowser, StrId::STR_BROWSE_FILES, Folder});
  menuEntries.push_back({MenuAction::Recents, StrId::STR_MENU_RECENT_BOOKS, Recent});
  if (!GLOBAL_BOOKMARKS.isEmpty()) {
    menuEntries.push_back({MenuAction::GlobalBookmarks, StrId::STR_GLOBAL_BOOKMARKS, Book});
  }
  if (hasOpdsServers) {
    menuEntries.push_back({MenuAction::OpdsBrowser, StrId::STR_OPDS_BROWSER, Library});
  }
  menuEntries.push_back({MenuAction::FileTransfer, StrId::STR_FILE_TRANSFER, Transfer});
  if (SETTINGS.useWeather) {
    menuEntries.push_back({MenuAction::Weather, StrId::STR_WEATHER, Weather});
  }
  menuEntries.push_back({MenuAction::Settings, StrId::STR_SETTINGS_TITLE, Settings});
  menuEntriesDirty = false;
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
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    // Check for a sidecar cover — takes priority over embedded cover.
    // Also catches books registered before sidecar support (empty coverBmpPath).
    const std::string sidecar = ReaderActivity::sidecarCoverPath(book.path);
    if (!sidecar.empty()) {
      const bool sidecarAlreadyStored = book.coverBmpPath == sidecar;
      LOG_DBG("HOME", "Sidecar for %s: stored=%s alreadyStored=%d", book.path.c_str(), book.coverBmpPath.c_str(),
              sidecarAlreadyStored ? 1 : 0);
      if (!sidecarAlreadyStored) {
        LOG_DBG("HOME", "Updating coverBmpPath to sidecar: %s", sidecar.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, sidecar);
        RecentBook updated = book;
        updated.coverBmpPath = sidecar;
        recentBooks.push_back(updated);
        continue;
      }
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;

  const auto thumbSizes = GUI.getCoverThumbSizes(coverHeight);

  for (; nextRecentCoverIndex < recentBooks.size(); nextRecentCoverIndex++) {
    RecentBook& book = recentBooks[nextRecentCoverIndex];
    if (!book.coverBmpPath.empty()) {
      // Sidecar covers (JPG/PNG paths stored directly) must be converted to BMP thumbnails
      // and the stored coverBmpPath updated to the cache path with [WIDTH]x[HEIGHT] placeholder.
      const bool isSidecar =
          FsHelpers::hasJpgExtension(book.coverBmpPath) || FsHelpers::hasPngExtension(book.coverBmpPath);
      if (isSidecar) {
        LOG_DBG("HOME", "Converting sidecar %s for book %s", book.coverBmpPath.c_str(), book.path.c_str());
        if (!Storage.exists(book.coverBmpPath.c_str())) {
          LOG_ERR("HOME", "Sidecar file missing: %s", book.coverBmpPath.c_str());
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
          book.coverBmpPath = "";
        } else {
          // Free the carousel frame cache before converting — PNG/JPEG decode needs ~42 KB
          // contiguous heap, which won't be available while the 48 KB frame buffer is held.
          // The cache will be rebuilt on the next render.
          UITheme::getInstance().getMutableTheme().invalidateFrameCache();

          const std::string cacheBase = ReaderActivity::bookCacheDir(book.path);
          const std::string placeholder = cacheBase + "/[HEIGHT].bmp";
          bool success = true;
          if (!thumbSizes.empty()) {
            for (const auto& sz : thumbSizes) {
              const std::string name = std::to_string(sz.first) + "x" + std::to_string(sz.second) + ".bmp";
              if (convertSidecarToBmp(cacheBase, book.coverBmpPath, sz.first, sz.second, name).empty()) {
                success = false;
                break;
              }
            }
          } else {
            const int w = coverHeight * 6 / 10;
            const std::string name = std::to_string(coverHeight) + ".bmp";
            if (convertSidecarToBmp(cacheBase, book.coverBmpPath, w, coverHeight, name).empty()) success = false;
          }
          if (success) {
            LOG_DBG("HOME", "Sidecar converted, placeholder: %s", placeholder.c_str());
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
            book.coverBmpPath = placeholder;
          } else {
            LOG_ERR("HOME", "Failed to convert sidecar cover for %s", book.path.c_str());
            // Don't permanently clear the path on failure — keep the raw sidecar path
            // so the next home visit can retry (e.g. after more memory becomes available).
          }
          coverRendered = false;
          nextRecentCoverIndex++;
          recentsLoading = false;
          requestUpdate();
          return;
        }
      }

      if (!book.coverBmpPath.empty()) {
        if (!thumbSizes.empty()) {
          // Theme uses WxH thumbnails — check which are missing and generate
          bool anyMissing = false;
          for (const auto& sz : thumbSizes) {
            const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, sz.first, sz.second);
            if (!Storage.exists(path.c_str())) {
              anyMissing = true;
              break;
            }
          }

          if (anyMissing) {
            bool success = true;
            if (FsHelpers::hasEpubExtension(book.path)) {
              Epub epub(book.path, "/.crosspoint");
              epub.load(false, true);
              for (const auto& sz : thumbSizes) {
                const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, sz.first, sz.second);
                if (!Storage.exists(path.c_str())) success = epub.generateThumbBmp(sz.first, sz.second) && success;
              }
            } else if (FsHelpers::hasXtcExtension(book.path)) {
              Xtc xtc(book.path, "/.crosspoint");
              if (xtc.load()) {
                for (const auto& sz : thumbSizes) {
                  const std::string path = UITheme::getCoverThumbPath(book.coverBmpPath, sz.first, sz.second);
                  if (!Storage.exists(path.c_str())) success = xtc.generateThumbBmp(sz.first, sz.second) && success;
                }
              }
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            nextRecentCoverIndex++;
            recentsLoading = false;
            requestUpdate();
            return;
          }
        } else {
          std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
          if (!Storage.exists(coverPath.c_str())) {
            if (FsHelpers::hasEpubExtension(book.path)) {
              Epub epub(book.path, "/.crosspoint");
              epub.load(false, true);
              bool success = epub.generateThumbBmp(coverHeight);
              if (!success) {
                RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
                book.coverBmpPath = "";
              }
              coverRendered = false;
              nextRecentCoverIndex++;
              recentsLoading = false;
              requestUpdate();
              return;
            } else if (FsHelpers::hasXtcExtension(book.path)) {
              Xtc xtc(book.path, "/.crosspoint");
              if (xtc.load()) {
                bool success = xtc.generateThumbBmp(coverHeight);
                if (!success) {
                  RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
                  book.coverBmpPath = "";
                }
                coverRendered = false;
                nextRecentCoverIndex++;
                recentsLoading = false;
                requestUpdate();
                return;
              }
            }
          }
        }
      }  // if (!book.coverBmpPath.empty()) after sidecar check
    }
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  selectorIndex = 0;
  recentsLoading = false;
  recentsLoaded = false;
  firstRenderDone = false;
  nextRecentCoverIndex = 0;
  coverRendered = false;
  freeCoverBuffer();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  if (recentBooks.empty()) {
    recentsLoaded = true;
  }

  // Apply focus: book path takes priority, else combined selector index (covers
  // "return to the menu entry I was on").
  bool focused = false;
  if (!focusBookPath.empty()) {
    for (size_t i = 0; i < recentBooks.size(); ++i) {
      if (recentBooks[i].path == focusBookPath) {
        selectorIndex = static_cast<int>(i);
        focused = true;
        break;
      }
    }
    focusBookPath.clear();
  }
  if (!focused && focusSelectorIndex >= 0) {
    rebuildMenuEntries();  // need menu count to clamp; rebuild is idempotent
    const int combinedSize = static_cast<int>(recentBooks.size() + menuEntries.size());
    if (combinedSize > 0) {
      selectorIndex = std::min(focusSelectorIndex, combinedSize - 1);
    }
  }
  focusSelectorIndex = -1;

  // Trigger first update
  menuEntriesDirty = true;
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
  UITheme::getInstance().getMutableTheme().invalidateFrameCache();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
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
  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  if (isCarousel) {
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuItemCount = static_cast<int>(menuEntries.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;
        selectorIndex = bookCount;
      } else {
        selectorIndex = lastCarouselBookIndex;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;
        selectorIndex = bookCount;
      } else {
        selectorIndex = lastCarouselBookIndex;
      }
      requestUpdate();
    }
  } else {
    const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());

    if (firstRenderDone && !recentsLoaded && !recentsLoading) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect contentRect = UITheme::getContentRect(renderer, true, false);
      const HomeScreenLayout layout =
          computeHomeScreenLayout(metrics, contentRect.height, static_cast<int>(menuEntries.size()));
      loadRecentCovers(getHomeCoverRenderHeight(layout));
      return;
    }

    buttonNavigator.onNext([this, totalItems] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, totalItems] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int recentsCount = static_cast<int>(recentBooks.size());
    if (selectorIndex < recentsCount) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIdx = selectorIndex - recentsCount;
      if (menuIdx < static_cast<int>(menuEntries.size())) {
        dispatchMenuAction(menuEntries[menuIdx].action);
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

  const int menuCount = static_cast<int>(menuEntries.size());
  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // Fast path: theme owns its own pre-rendered frame cache
  if (isCarousel) {
    const auto carouselLabels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    const bool handled = GUI.tryFastHomeRender(
        renderer, recentBooks, selectorIndex, menuCount,
        [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
        [this](int index) { return menuEntries[index].icon; }, carouselLabels.btn1, carouselLabels.btn2,
        carouselLabels.btn3, carouselLabels.btn4);
    if (handled) {
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.homeTopPadding}, nullptr);

  const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());
  if (selectorIndex >= totalItems) {
    selectorIndex = std::max(0, totalItems - 1);
  }

  const HomeScreenLayout layout = computeHomeScreenLayout(metrics, contentRect.height, menuCount);

  GUI.drawRecentBookCover(renderer,
                          Rect{contentRect.x, metrics.homeTopPadding, contentRect.width, layout.recentTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  GUI.drawButtonMenu(
      renderer,
      Rect{contentRect.x, metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap, contentRect.width,
           layout.menuHeight},
      menuCount, selectorIndex - static_cast<int>(recentBooks.size()),
      [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
      [this](int index) { return menuEntries[index].icon; });

  const auto labels = isCarousel ? mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                                 : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(getHomeCoverRenderHeight(computeHomeScreenLayout(metrics, contentRect.height, menuCount)));
  }
}

void HomeActivity::onSelectBook(const std::string& path) {
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectName = path;  // used to re-focus the book in the recents strip after return
  activityManager.replaceWithReader(path, std::move(hint));
}

void HomeActivity::dispatchMenuAction(MenuAction action) {
  // Record where the menu entry was focused so that when the launched activity exits
  // (via returnFromChild() or an empty-stack finish()), we come back to the same row.
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectIndex = selectorIndex;
  activityManager.setReturnHint(std::move(hint));

  switch (action) {
    case MenuAction::FileBrowser:
      activityManager.goToFileBrowser();
      break;
    case MenuAction::Recents:
      activityManager.goToRecentBooks();
      break;
    case MenuAction::GlobalBookmarks:
      activityManager.goToGlobalBookmarks();
      break;
    case MenuAction::OpdsBrowser:
      activityManager.goToBrowser();
      break;
    case MenuAction::FileTransfer:
      activityManager.goToFileTransfer();
      break;
    case MenuAction::Weather:
      activityManager.goToWeather();
      break;
    case MenuAction::Settings:
      activityManager.goToSettings();
      break;
    default:
      LOG_ERR("HOME", "Unexpected menu action: %d", static_cast<int>(action));
      break;
  }
}
