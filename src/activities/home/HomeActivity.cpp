#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
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

#if defined(CROSSPOINT_FREEINKUI_HOME) && CROSSPOINT_FREEINKUI_HOME
#include <FreeInkUIGfxRenderer.h>
#endif

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

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  coverSelectorIndex = recentBooks.empty() ? 0 : std::min(selectorIndex, static_cast<int>(recentBooks.size()) - 1);

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
  coverBufferStripSelected = selectorIndex < static_cast<int>(recentBooks.size());
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

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      coverSelectorIndex = selectorIndex;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      coverSelectorIndex = selectorIndex;
    }
    requestUpdate();
  });

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

#if defined(CROSSPOINT_FREEINKUI_HOME) && CROSSPOINT_FREEINKUI_HOME
void HomeActivity::renderFreeInkUI() {
  freeink::ui::GfxRendererTarget draw(renderer);
  draw.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  draw.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  draw.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, UI_12_FONT_ID);

  const freeink::ui::ThemeTokens tokens = freeink::ui::defaultThemeTokens(
      freeink::ui::GfxRendererTarget::FONT_SMALL, freeink::ui::GfxRendererTarget::FONT_BODY, freeink::ui::GfxRendererTarget::FONT_TITLE);

  // Render-only frame: button input keeps flowing through loop()'s
  // ButtonNavigator/selectorIndex, so the snapshot stays empty and finish()
  // is never called. Touch routing hooks in here once HalGPIO exposes raw
  // touch points.
  freeink::ui::InteractionBuffer<24> interactions;
  const freeink::ui::InputSnapshot input{};
  const freeink::ui::DeviceContext device = draw.deviceContext();
  freeink::ui::Frame<24> ui(draw, device, input, interactions);

  renderer.clearScreen();

  freeink::ui::Stack<3> screen(ui.safeRect(), freeink::ui::Axis::Column, 0);
  screen.fixed(tokens.headerHeight);
  screen.flex(1);
  screen.fixed(tokens.footerHeight);
  screen.layout();

  freeink::ui::HeaderProps header;
  header.title = !recentBooks.empty()
                     ? recentBooks[std::min(coverSelectorIndex, static_cast<int>(recentBooks.size()) - 1)].title.c_str()
                     : "CrossPoint";
  header.titleText = tokens.titleText;
  freeink::ui::header(ui, screen.rect(0), header);

  // One flat list over recent books + static menu — the same index space
  // loop() drives with selectorIndex (recents first, then menu entries in
  // indexToMenuItem() order).
  constexpr uint16_t MAX_ROWS = 16;
  freeink::ui::ListItem items[MAX_ROWS]{};
  uint16_t count = 0;
  for (const RecentBook& book : recentBooks) {
    if (count >= MAX_ROWS) break;
    items[count].label = book.title.c_str();
    items[count].actionValue = static_cast<int16_t>(count);
    items[count].enabled = true;
    ++count;
  }
  const char* menuLabels[5];
  uint8_t menuCount = 0;
  menuLabels[menuCount++] = tr(STR_BROWSE_FILES);
  menuLabels[menuCount++] = tr(STR_MENU_RECENT_BOOKS);
  if (hasOpdsServers) menuLabels[menuCount++] = tr(STR_OPDS_BROWSER);
  menuLabels[menuCount++] = tr(STR_FILE_TRANSFER);
  menuLabels[menuCount++] = tr(STR_SETTINGS_TITLE);
  for (uint8_t i = 0; i < menuCount && count < MAX_ROWS; ++i, ++count) {
    items[count].label = menuLabels[i];
    items[count].actionValue = static_cast<int16_t>(count);
    items[count].enabled = true;
  }

  freeink::ui::ListProps list;
  list.items = items;
  list.count = count;
  list.selectedIndex = static_cast<int16_t>(selectorIndex);
  list.rowHeight = tokens.rowHeight;
  list.labelText = tokens.bodyText;
  list.sidePadding = tokens.spaceMd;
  const uint16_t visible = freeink::ui::listVisibleRows(screen.rect(1), list.rowHeight, list.rowGap);
  list.topIndex = freeink::ui::listTopIndexFor(list.selectedIndex, 0, visible, count);
  freeink::ui::list(ui, screen.rect(1), list);

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const char* hintLabels[4] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};
  freeink::ui::Stack<4> hints(screen.rect(2), freeink::ui::Axis::Row, tokens.spaceSm);
  for (uint8_t i = 0; i < 4; ++i) hints.flex(1);
  hints.layout();
  for (uint8_t i = 0; i < 4; ++i) {
    if (hintLabels[i] == nullptr || hintLabels[i][0] == '\0') continue;
    freeink::ui::ButtonProps hint;
    hint.label = hintLabels[i];
    hint.text = tokens.smallText;
    freeink::ui::button(ui, hints.rect(i), hint);
  }
}
#endif  // CROSSPOINT_FREEINKUI_HOME

void HomeActivity::render(RenderLock&&) {
#if defined(CROSSPOINT_FREEINKUI_HOME) && CROSSPOINT_FREEINKUI_HOME
  renderFreeInkUI();
  renderer.displayBuffer();
  firstRenderDone = true;
  return;
#endif
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int coverCacheBleed = 12;
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
  const bool coverStripSelected = selectorIndex < static_cast<int>(recentBooks.size());
  bool bufferRestored = hasCoverArea && coverBufferStored &&
                        (!selectorSensitiveCoverCache || (coverBufferSelectorIndex == coverSelectorIndex &&
                                                          coverBufferStripSelected == coverStripSelected)) &&
                        restoreCoverBuffer();

  if (hasCoverArea) {
    GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                            recentBooks, coverSelectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this), coverStripSelected);
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
  } else if (metrics.homeCoverHeight > 0 && !recentsLoaded && !recentsLoading) {
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
