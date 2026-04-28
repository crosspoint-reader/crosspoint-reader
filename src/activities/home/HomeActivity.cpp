#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed explicitly in onSelectBook() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
uint8_t* gCachedFrames[HomeActivity::kCarouselFrameCount] = {};
int gCachedFrameBookIdx[HomeActivity::kCarouselFrameCount] = {-1, -1, -1};
int gCachedFrameCount = 0;
std::string gCacheKey;

int findFrameSlot(int bookIdx) {
  for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
    if (gCachedFrameBookIdx[i] == bookIdx && gCachedFrames[i] != nullptr) return i;
  }
  return -1;
}

void invalidateCarouselCache() {
  for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
    if (gCachedFrames[i]) {
      free(gCachedFrames[i]);
      gCachedFrames[i] = nullptr;
    }
    gCachedFrameBookIdx[i] = -1;
  }
  gCachedFrameCount = 0;
  gCacheKey.clear();
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
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

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  // Tracks which book indices had a thumbnail generated this pass.
  // Sized to LyraCarousel's max (5) since recentBooks is bounded by the active
  // theme's homeRecentBooksCount and LyraCarousel is the maximum across themes.
  static_assert(LyraCarouselMetrics::values.homeRecentBooksCount == 5,
                "bookUpdated array sized to LyraCarousel max; if this metric "
                "changes or another theme exceeds it, resize the array.");
  bool bookUpdated[LyraCarouselMetrics::values.homeRecentBooksCount] = {};
  Rect popupRect;

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      if (isCarouselTheme) {
        // For carousel: generate exact-size thumbnails for center and side slots.
        // Load the source image once even when both sizes are missing.
        const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterCoverW,
                                                                  LyraCarouselTheme::kCenterCoverH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                LyraCarouselTheme::kSideCoverH);
        const bool centerMissing = !Storage.exists(centerPath.c_str());
        const bool sideMissing = !Storage.exists(sidePath.c_str());

        if (centerMissing || sideMissing) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            epub.load(false, true);
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            if (centerMissing)
              success =
                  epub.generateThumbBmp(LyraCarouselTheme::kCenterCoverW, LyraCarouselTheme::kCenterCoverH) && success;
            if (sideMissing)
              success =
                  epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            } else {
              bookUpdated[progress] = true;
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
              bool success = true;
              if (centerMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kCenterCoverW, LyraCarouselTheme::kCenterCoverH) && success;
              if (sideMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
              if (!success) {
                RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
                book.coverBmpPath = "";
              } else {
                bookUpdated[progress] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      } else {
        // Non-carousel: generate height-keyed thumbnail
        std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (!Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            epub.load(false, true);
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = epub.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            } else {
              bookUpdated[progress] = true;  // non-carousel path reuses same tracking
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              if (!showingLoading) {
                showingLoading = true;
                popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
              }
              GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
              bool success = xtc.generateThumbBmp(coverHeight);
              if (!success) {
                RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
                book.coverBmpPath = "";
              } else {
                bookUpdated[progress] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Re-render only the affected slots rather than rebuilding the entire cache.
  if (isCarouselTheme) {
    bool anyUpdated = false;
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (!bookUpdated[i]) continue;
      anyUpdated = true;
      if (carouselFramesReady) {
        // Only re-render the slot holding this book; books outside the window
        // will be picked up by updateSlidingWindowCache on next navigation.
        const int slot = findFrameSlot(i);
        if (slot >= 0) renderCarouselFrame(i, slot);
      }
    }
    if (anyUpdated) {
      if (!carouselFramesReady) {
        // Cache not yet initialised (shouldn't happen) — fall back to full render.
        preRenderCarouselFrames();
      }
      requestUpdate();
    }
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  selectorIndex = 0;
  carouselFramesReady = false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // Pre-render carousel frames before the first display update so the fast
  // path is active from render #1. Cache hit = instant. Cache miss = SD reads
  // here, but the E-ink is still refreshing from the previous activity anyway.
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
    preRenderCarouselFrames();
  }

  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  freeCoverBuffer();
  invalidateCarouselCache();
  freeCarouselFrames();
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

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

void HomeActivity::preRenderCarouselFrames() {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return;

  // Build cache key from book paths in order
  std::string newKey;
  newKey.reserve(128);
  for (const auto& b : recentBooks) {
    newKey += b.path;
    newKey += '\0';
  }

  // Cache hit: same books in same order — reuse without any SD reads
  if (newKey == gCacheKey && gCachedFrameCount > 0) {
    for (int i = 0; i < gCachedFrameCount; ++i) carouselFrames[i] = gCachedFrames[i];
    carouselFramesReady = true;
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  // Cache miss: free old cache and re-render
  invalidateCarouselCache();

  if (!renderer.getFrameBuffer()) return;

  const size_t bufferSize = renderer.getBufferSize();
  freeCoverBuffer();  // reclaim 48KB before allocating frames

  const int frameCount = std::min(bookCount, kCarouselFrameCount);
  for (int i = 0; i < frameCount; ++i) {
    gCachedFrames[i] = static_cast<uint8_t*>(malloc(bufferSize));
    if (!gCachedFrames[i]) {
      LOG_ERR("HOME", "preRenderCarouselFrames: malloc failed for frame %d", i);
      invalidateCarouselCache();
      return;
    }
  }

  // Render only the currently-selected cover. Adjacent frames are populated
  // lazily by updateSlidingWindowCache() after the first paint completes,
  // so the user sees their book immediately and scroll latency only appears
  // once they actually navigate.
  const int selectedBookIdx = (selectorIndex < bookCount) ? selectorIndex : lastCarouselBookIndex;
  const int initialBookIdx = (selectedBookIdx >= 0 && selectedBookIdx < bookCount) ? selectedBookIdx : 0;
  renderCarouselFrame(initialBookIdx, 0);

  gCachedFrameCount = frameCount;
  gCacheKey = newKey;
  carouselFramesReady = true;
  coverRendered = false;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  if (isCarousel) {
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuItemCount = 4 + (hasOpdsServers ? 1 : 0);
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
    const int menuCount = getMenuItemCount();
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsServers ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
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

  // Fast path: pre-rendered frames ready — memcpy + border overlay
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    const int slotIdx = findFrameSlot(centerIdx);

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());

      GUI.drawCarouselBorder(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                             inCarouselRow);

      std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                            tr(STR_SETTINGS_TITLE)};
      std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};
      if (hasOpdsServers) {
        menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
        menuIcons.insert(menuIcons.begin() + 2, Library);
      }

      GUI.drawButtonMenu(
          renderer,
          Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
               pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                             metrics.buttonHintsHeight)},
          static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
          [&menuItems](int index) { return std::string(menuItems[index]); },
          [&menuIcons](int index) { return menuIcons[index]; });

      const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

      renderer.displayBuffer();
      // E-ink refresh complete — pre-render the missing adjacent frame while idle.
      updateSlidingWindowCache(centerIdx, bookCount);
      // Mirror the slow-path trigger: generate missing thumbnails on the second
      // render so the E-ink is already showing something before the SD work starts.
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
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

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const auto labels = isCarouselTheme ? mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                                      : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
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

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCachedFrames[slotIdx]) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool dummy1 = false, dummy2 = false, dummy3 = false;

  // selectorIndex = bookCount → drawRecentBookCover uses lastCarouselSelectorIndex (set below)
  // and draws no selection border.
  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, bookCount, dummy1, dummy2, dummy3, []() { return true; });

  memcpy(gCachedFrames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCachedFrameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCachedFrames[slotIdx];
}

void HomeActivity::updateSlidingWindowCache(int centerIdx, int bookCount) {
  // No sliding needed when all books already fit in the cache.
  if (bookCount <= kCarouselFrameCount || !carouselFramesReady) return;

  const int prevIdx = (centerIdx + bookCount - 1) % bookCount;
  const int nextIdx = (centerIdx + 1) % bookCount;

  const bool hasPrev = findFrameSlot(prevIdx) >= 0;
  const bool hasNext = findFrameSlot(nextIdx) >= 0;
  if (hasPrev && hasNext) return;  // window already complete

  const int missingIdx = !hasPrev ? prevIdx : nextIdx;

  // Evict the slot whose book is furthest from centerIdx (never evict center itself,
  // nor the adjacent that is already cached).
  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCachedFrames[i]) continue;
    const int bookInSlot = gCachedFrameBookIdx[i];
    if (bookInSlot == centerIdx) continue;
    if (hasPrev && bookInSlot == prevIdx) continue;
    if (hasNext && bookInSlot == nextIdx) continue;
    const int diff = std::abs(bookInSlot - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }

  if (evictSlot >= 0) {
    LOG_DBG("HOME", "carousel: evict slot %d (book %d) -> book %d", evictSlot, gCachedFrameBookIdx[evictSlot],
            missingIdx);
    renderCarouselFrame(missingIdx, evictSlot);
  }
}

void HomeActivity::onSelectBook(const std::string& path) {
  invalidateCarouselCache();
  freeCarouselFrames();
  activityManager.goToReader(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
