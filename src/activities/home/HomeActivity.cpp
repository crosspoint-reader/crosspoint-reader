#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (slotItem() != HomeMenuItem::NONE) {
    count++;
  }
  return count;
}

int HomeActivity::recentPageStart(const int booksPerPage, const int selectedIndex) const {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(recentBooks.size())) return 0;
  return (selectedIndex / booksPerPage) * booksPerPage;
}

void HomeActivity::setSelection(const int index) {
  selectorIndex.store(index, std::memory_order_relaxed);
  if (index >= 0 && index < static_cast<int>(recentBooks.size())) {
    lastRecentBookIndex = index;
  }
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

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

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(const int coverHeight, const int firstBook, const int bookCount) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  const int lastBook = std::min(firstBook + bookCount, static_cast<int>(recentBooks.size()));
  for (int bookIndex = firstBook; bookIndex < lastBook; ++bookIndex) {
    RecentBook& book = recentBooks[bookIndex];
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / bookCount);
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          const bool cacheLoaded = epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / bookCount));
          // Without a book cache (e.g. after "Delete Book Cache") there is no cached metadata,
          // so read the cover location straight from the EPUB.
          bool success =
              cacheLoaded ? epub.generateThumbBmp(thumbHeight) : epub.generateThumbBmpFromSource(thumbHeight);
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
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / bookCount));
            bool success = xtc.generateThumbBmp(thumbHeight);
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
  coversLoadedPageStart = firstBook;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  const int recentBookLimit =
      coverGridUi ? CoverGridHomeUi::MAX_BOOKS
                  : metrics.homeRecentBooksCount > 1 ? RecentBooksStore::MAX_RECENT_BOOKS : metrics.homeRecentBooksCount;
  loadRecentBooks(recentBookLimit);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    const HomeMenuItem slot = slotItem();
    coverGridUi->begin(recentBooks,
                       slot == HomeMenuItem::PLUGINS        ? CoverGridHomeUi::SlotTab::PLUGINS
                       : slot == HomeMenuItem::OPDS_BROWSER ? CoverGridHomeUi::SlotTab::OPDS
                                                            : CoverGridHomeUi::SlotTab::NONE,
                       hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  setSelection(initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, slotItem()));
  displayedRecentPageStart.store(0, std::memory_order_relaxed);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  if (needed > coverBufferSize) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(needed);
    if (!buffer) {
      LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", static_cast<unsigned>(needed));
      return false;
    }
    coverBuffer = std::move(buffer);
    coverBufferSize = needed;
  }
  coverBufferInverted = display.isInverted();
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(), coverBufferSize)) {
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(), coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  coverBuffer.reset();
  coverBufferSize = 0;
  coverBufferStored = false;
  coverBufferPageStart = -1;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);
    if (selectedIndex < static_cast<int>(recentBooks.size())) {
      onRecentBookSelected(recentBooks[selectedIndex].path);
      return;
    }
    const int menuIndex = selectedIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, slotItem())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::PLUGINS:
        onPluginsOpen();
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
  };

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi) {
    if (metrics.homeRecentBooksCount > 1) {
      const auto changeCoverSelection = [this](const int direction) {
        const int bookCount = static_cast<int>(recentBooks.size());
        if (bookCount == 0) return;
        const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);
        if (selectedIndex >= bookCount) return;

        const int nextIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedIndex, bookCount)
                                            : ButtonNavigator::previousIndex(selectedIndex, bookCount);
        setSelection(nextIndex);
        requestUpdate();
      };
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenRight},
                                           [&changeCoverSelection] { changeCoverSelection(+1); });
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenLeft},
                                           [&changeCoverSelection] { changeCoverSelection(-1); });
      const auto moveToNextHomeRow = [this, menuCount] {
        const int bookCount = static_cast<int>(recentBooks.size());
        const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);
        const int nextIndex = bookCount > 0 && selectedIndex < bookCount
                                  ? bookCount
                                  : ButtonNavigator::nextIndex(selectedIndex, menuCount);
        setSelection(nextIndex);
        requestUpdate();
      };
      const auto moveToPreviousHomeRow = [this, menuCount] {
        const int bookCount = static_cast<int>(recentBooks.size());
        const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);
        if (selectedIndex < bookCount) return;

        const int previousIndex = selectedIndex == bookCount
                                      ? lastRecentBookIndex
                                      : ButtonNavigator::previousIndex(selectedIndex, menuCount);
        setSelection(previousIndex);
        requestUpdate();
      };
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenDown}, moveToNextHomeRow);
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ScreenUp}, moveToPreviousHomeRow);
    } else {
      buttonNavigator.onNext([this, menuCount] {
        setSelection(ButtonNavigator::nextIndex(selectorIndex.load(std::memory_order_relaxed), menuCount));
        requestUpdate();
      });

      buttonNavigator.onPrevious([this, menuCount] {
        setSelection(ButtonNavigator::previousIndex(selectorIndex.load(std::memory_order_relaxed), menuCount));
        requestUpdate();
      });
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    setSelection(ButtonNavigator::nextIndex(selectorIndex.load(std::memory_order_relaxed), menuCount));
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    setSelection(ButtonNavigator::previousIndex(selectorIndex.load(std::memory_order_relaxed), menuCount));
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onRecentBookSelected(recentBooks[0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      setSelection(touched);
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex.load(std::memory_order_relaxed) - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      setSelection(base + idx);
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int firstRecentBook = displayedRecentPageStart.load(std::memory_order_relaxed);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()) - firstRecentBook, coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth,
                                               recentCount, metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    const int selectedBook = firstRecentBook + touchedBook;
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectedIndex != selectedBook) {
        setSelection(selectedBook);
        requestUpdate();
      }
    } else {
      setSelection(selectedBook);
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectedIndex != touchedIndex) {
        setSelection(touchedIndex);
        requestUpdate();
      }
    } else {
      setSelection(touchedIndex);
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int selectedIndex = selectorIndex.load(std::memory_order_relaxed);

  renderer.clearScreen();
  if (coverGridUi) {
    const auto drawGrid = [&] {
      renderer.clearScreen();
      coverGridUi->setSelection(selectedIndex);
      UITheme::getInstance().drawCoverGridHome(*coverGridUi);
      // Front Left/Right walk the tabs, so their hints read Left/Right; the
      // side page buttons (unhinted) walk the covers.
      const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                                tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    };
    drawGrid();
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Resolve the covers before the
    // panel is touched and redraw into the framebuffer, so Home paints once
    // instead of three e-ink refreshes (placeholder, placeholder, covers).
    if (coverGridUi->takeThumbHeightChanged()) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT, 0, static_cast<int>(recentBooks.size()));
      coverGridUi->refreshCoverPaths();
      drawGrid();
      if (coverGridUi->takeThumbHeightChanged()) {
        coverGridUi->refreshCoverPaths();
        recentsLoaded = false;
        requestUpdate();
      }
    }
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    firstRenderDone = true;
    return;
  }
  // A cover snapshot from the other output polarity holds the other byte form (night mode keeps images pre-inverted so
  // they stay positive): pasting it back after a theme switch showed the cover as a negative. Redraw it instead.
  if (coverBufferStored && coverBufferInverted != display.isInverted()) {
    freeCoverBuffer();
    coverRendered = false;
  }
  const int booksPerPage = std::max(1, metrics.homeRecentBooksCount);
  const int firstRecentBook = selectedIndex < static_cast<int>(recentBooks.size())
                                   ? recentPageStart(booksPerPage, selectedIndex)
                                   : displayedRecentPageStart.load(std::memory_order_relaxed);
  const int booksOnPage = std::min(booksPerPage, static_cast<int>(recentBooks.size()) - firstRecentBook);
  if (coverBufferStored && coverBufferPageStart != firstRecentBook) {
    coverBufferStored = false;
    coverRendered = false;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight},
                          recentBooks, firstRecentBook, selectedIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));
  if (coverBufferStored) coverBufferPageStart = firstRecentBook;

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};

  if (slotItem() != HomeMenuItem::NONE) {
    menuItems.insert(menuItems.begin() + 2, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, hasPlugins ? Plugins : Book);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, content.width,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectedIndex : selectedIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = metrics.homeRecentBooksCount > 1
                          ? mappedInput.mapDirectionalLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                                             tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP),
                                                             tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                                                  tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  displayedRecentPageStart.store(firstRecentBook, std::memory_order_relaxed);

  firstRenderDone = true;
  if ((!recentsLoaded || coversLoadedPageStart != firstRecentBook) && !recentsLoading && booksOnPage > 0) {
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight, firstRecentBook, booksOnPage);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(); }
