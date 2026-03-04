#include "HomeActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "SpiBusMutex.h"
#include "activities/TaskShutdown.h"
#include "components/UITheme.h"
#include "core/features/FeatureModules.h"
#include "fontIds.h"
#include "util/ForkDriftNavigation.h"
#include "util/StringUtils.h"

int HomeActivity::getMenuItemCount() const {
  int count = 3;  // My Library, File transfer, Settings
  if (hasContinueReading) count++;
  if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::OpdsBrowser, hasOpdsUrl)) count++;
  if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false)) count++;
  if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false)) count++;
  return count;
}

std::string HomeActivity::fallbackTitleFromPath(const std::string& path) {
  auto title = path;
  const size_t lastSlash = title.find_last_of('/');
  if (lastSlash != std::string::npos) {
    title = title.substr(lastSlash + 1);
  }

  if (StringUtils::checkFileExtension(title, ".xtch")) {
    title.resize(title.length() - 5);
  } else if (StringUtils::checkFileExtension(title, ".epub") || StringUtils::checkFileExtension(title, ".xtc") ||
             StringUtils::checkFileExtension(title, ".txt") || StringUtils::checkFileExtension(title, ".md")) {
    title.resize(title.length() - 4);
  }

  return title;
}

std::string HomeActivity::fallbackAuthor(const RecentBook& book) {
  if (!book.author.empty()) {
    return book.author;
  }
  return "";
}

bool HomeActivity::isPokemonPartyHomeMode() const {
  return SETTINGS.uiTheme == CrossPointSettings::FORK_DRIFT &&
         core::FeatureModules::hasCapability(core::Capability::PokemonParty);
}

void HomeActivity::rebuildMenuLayout() {
  const bool forkDrift = SETTINGS.uiTheme == CrossPointSettings::FORK_DRIFT;
  if (forkDrift) {
    if (isPokemonPartyHomeMode()) {
      menuOpenBookIndex = -1;
      menuMyLibraryIndex = -1;
      menuOpdsIndex = -1;
      menuTodoIndex = -1;
      menuAnkiIndex = -1;
      menuFileTransferIndex = -1;
      menuSettingsIndex = 0;
      menuItemCount = 1;
      return;
    }
    menuOpenBookIndex = -1;
    int idx = 0;
    menuMyLibraryIndex = idx++;
    menuOpdsIndex = -1;
    menuTodoIndex =
        core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false) ? idx++ : -1;
    menuAnkiIndex =
        core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false) ? idx++ : -1;
    menuFileTransferIndex = idx++;
    menuSettingsIndex = idx++;
    menuItemCount = idx;
    return;
  }
  int idx = 0;
  menuOpenBookIndex = idx++;
  menuMyLibraryIndex = idx++;
  menuOpdsIndex =
      core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::OpdsBrowser, hasOpdsUrl) ? idx++ : -1;
  menuTodoIndex =
      core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false) ? idx++ : -1;
  menuAnkiIndex =
      core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false) ? idx++ : -1;
  menuFileTransferIndex = idx++;
  menuSettingsIndex = idx++;
  menuItemCount = idx;
}

void HomeActivity::loadRecentBooks() {
  auto metrics = UITheme::getInstance().getMetrics();
  const int maxBooks = metrics.homeRecentBooksCount;

  recentBooks.clear();
  const auto& storedBooks = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<size_t>(maxBooks), storedBooks.size()));

  for (const auto& stored : storedBooks) {
    if (recentBooks.size() >= static_cast<size_t>(maxBooks)) {
      break;
    }

    RecentBook entry = stored;
    if (entry.title.empty()) {
      entry.title = fallbackTitleFromPath(entry.path);
      if (entry.title != stored.title) {
        RECENT_BOOKS.updateBook(entry.path, entry.title, entry.author, entry.coverBmpPath);
      }
    }
    recentBooks.push_back(entry);
  }

  if (recentBooks.empty()) {
    selectedBookIndex = 0;
    return;
  }

  if (!APP_STATE.openEpubPath.empty()) {
    for (size_t i = 0; i < recentBooks.size(); ++i) {
      if (recentBooks[i].path == APP_STATE.openEpubPath) {
        selectedBookIndex = static_cast<int>(i);
        return;
      }
    }
  }

  selectedBookIndex = std::min(selectedBookIndex, static_cast<int>(recentBooks.size()) - 1);
  selectedBookIndex = std::max(0, selectedBookIndex);
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  SpiBusMutex::Guard guard;
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (size_t i = 0; i < recentBooks.size(); ++i) {
    RecentBook& book = recentBooks[i];
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        const auto homeCardData = core::FeatureModules::resolveHomeCardData(book.path, coverHeight);
        if (homeCardData.handled) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));

          if (!homeCardData.coverPath.empty()) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, homeCardData.coverPath);
            book.coverBmpPath = homeCardData.coverPath;
          } else if (homeCardData.loaded) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        }
      }
    }
    progress++;
  }
  recentsLoading = false;
  recentsLoaded = true;
}

void HomeActivity::openSelectedBook() {
  if (recentBooks.empty()) {
    return;
  }

  if (selectedBookIndex < 0 || selectedBookIndex >= static_cast<int>(recentBooks.size())) {
    selectedBookIndex = 0;
  }

  const auto& selected = recentBooks[static_cast<size_t>(selectedBookIndex)];
  if (!Storage.exists(selected.path.c_str())) {
    loadRecentBooks();
    requestUpdate();
    return;
  }

  APP_STATE.openEpubPath = selected.path;
  APP_STATE.saveToFile();
  onContinueReading();
}

std::string HomeActivity::getMenuItemLabel(const int index) const {
  if (isPokemonPartyHomeMode()) {
    if (index == menuSettingsIndex) {
      return "Settings";
    }
    return "";
  }
  if (index == menuOpenBookIndex) {
    return recentBooks.empty() ? "Open Book (empty)" : "Open Book";
  }
  if (index == menuMyLibraryIndex) {
    return "My Library";
  }
  if (index == menuOpdsIndex) {
    return "OPDS Browser";
  }
  if (index == menuTodoIndex) {
    return "TODO";
  }
  if (index == menuAnkiIndex) {
    return "Anki";
  }
  if (index == menuFileTransferIndex) {
    return "File Transfer";
  }
  if (index == menuSettingsIndex) {
    return "Settings";
  }
  return "";
}

bool HomeActivity::drawCoverAt(const std::string& coverPath, const int x, const int y, const int width,
                               const int height) const {
  if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
    return false;
  }

  SpiBusMutex::Guard guard;
  FsFile file;
  if (!Storage.openFileForRead("HOME", coverPath, file)) {
    return false;
  }

  Bitmap bitmap(file);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    renderer.drawBitmap(bitmap, x, y, width, height);
  }
  file.close();
  return ok;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;
  const bool mediaPickerEnabled = core::FeatureModules::hasCapability(core::Capability::HomeMediaPicker);

  if (mediaPickerEnabled) {
    loadRecentBooks();
    rebuildMenuLayout();
    selectedMenuIndex = 0;
    inButtonGrid = (SETTINGS.uiTheme == CrossPointSettings::FORK_DRIFT && recentBooks.empty());
    hasContinueReading = !recentBooks.empty();
    selectorIndex = 0;

    hasCoverImage = false;
    coverRendered = false;
    coverBmpPath.clear();
    lastBookTitle.clear();
    lastBookAuthor.clear();
  } else {
    // Check if we have a book to continue reading
    hasContinueReading = !APP_STATE.openEpubPath.empty() && Storage.exists(APP_STATE.openEpubPath.c_str());

    if (hasContinueReading) {
      // Extract filename from path for display
      lastBookTitle = APP_STATE.openEpubPath;
      const size_t lastSlash = lastBookTitle.find_last_of('/');
      if (lastSlash != std::string::npos) {
        lastBookTitle = lastBookTitle.substr(lastSlash + 1);
      }

      const int thumbHeight = renderer.getScreenHeight() / 2;
      const auto homeCardData = core::FeatureModules::resolveHomeCardData(APP_STATE.openEpubPath, thumbHeight);
      if (!homeCardData.title.empty()) {
        lastBookTitle = homeCardData.title;
      }
      if (!homeCardData.author.empty()) {
        lastBookAuthor = homeCardData.author;
      }
      if (!homeCardData.coverPath.empty()) {
        coverBmpPath = homeCardData.coverPath;
        hasCoverImage = true;
      }

      // Preserve previous xtc fallback behavior when metadata is unavailable.
      if (homeCardData.handled && homeCardData.title.empty()) {
        if (StringUtils::checkFileExtension(lastBookTitle, ".xtch")) {
          lastBookTitle.resize(lastBookTitle.length() - 5);
        } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
          lastBookTitle.resize(lastBookTitle.length() - 4);
        }
      }
    }

    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  recentBooks.clear();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  coverBufferStored = true;
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

  const size_t bufferSize = GfxRenderer::getBufferSize();
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
  const bool mediaPickerEnabled = core::FeatureModules::hasCapability(core::Capability::HomeMediaPicker);

  if (mediaPickerEnabled) {
    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    const bool rightPressed = mappedInput.wasPressed(MappedInputManager::Button::Right);
    const bool upPressed = mappedInput.wasPressed(MappedInputManager::Button::Up);
    const bool downPressed = mappedInput.wasPressed(MappedInputManager::Button::Down);
    const bool forkDrift = SETTINGS.uiTheme == CrossPointSettings::FORK_DRIFT;
    const bool pokemonPartyHomeMode = isPokemonPartyHomeMode();

    if (pokemonPartyHomeMode && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      activityManager.goToRecentBooks();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (forkDrift) {
        if (inButtonGrid) {
          if (selectedMenuIndex == menuSettingsIndex) {
            onSettingsOpen();
            return;
          }
          if (!pokemonPartyHomeMode) {
            if (selectedMenuIndex == menuMyLibraryIndex) {
              onMyLibraryOpen();
              return;
            }
            if (selectedMenuIndex == menuTodoIndex) {
              onTodoOpen();
              return;
            }
            if (selectedMenuIndex == menuAnkiIndex) {
              onAnkiOpen();
              return;
            }
            if (selectedMenuIndex == menuFileTransferIndex) {
              onFileTransferOpen();
              return;
            }
          }
        } else if (!recentBooks.empty()) {
          openSelectedBook();
          return;
        }
      } else {
        if (selectedMenuIndex == menuOpenBookIndex) {
          openSelectedBook();
          return;
        }
        if (selectedMenuIndex == menuMyLibraryIndex) {
          onMyLibraryOpen();
          return;
        }
        if (selectedMenuIndex == menuOpdsIndex) {
          onOpdsBrowserOpen();
          return;
        }
        if (selectedMenuIndex == menuTodoIndex) {
          onTodoOpen();
          return;
        }
        if (selectedMenuIndex == menuAnkiIndex) {
          onAnkiOpen();
          return;
        }
        if (selectedMenuIndex == menuFileTransferIndex) {
          onFileTransferOpen();
          return;
        }
        if (selectedMenuIndex == menuSettingsIndex) {
          onSettingsOpen();
          return;
        }
      }
    }

    if (forkDrift) {
      constexpr int coverCols = 3;
      const int bookCount = static_cast<int>(recentBooks.size());
      const int coverRows = bookCount > 3 ? 2 : 1;

      if (inButtonGrid) {
        constexpr int btnCols = 2;
        constexpr int btnRows = 2;
        if (!pokemonPartyHomeMode && (leftPressed || rightPressed)) {
          int col = selectedMenuIndex % btnCols;
          int row = selectedMenuIndex / btnCols;
          if (leftPressed)
            col = (col + btnCols - 1) % btnCols;
          else
            col = (col + 1) % btnCols;
          selectedMenuIndex = row * btnCols + col;
          requestUpdate();
        }
        if (upPressed) {
          const int row = selectedMenuIndex / 2;
          if (row == 0 && bookCount > 0) {
            inButtonGrid = false;
            selectedBookIndex = std::min(selectedBookIndex, bookCount - 1);
            selectedBookIndex = std::max(0, selectedBookIndex);
            requestUpdate();
          } else if (!pokemonPartyHomeMode && row > 0) {
            selectedMenuIndex -= 2;
            requestUpdate();
          }
        } else if (!pokemonPartyHomeMode && downPressed) {
          int row = selectedMenuIndex / 2;
          if (row < btnRows - 1) {
            selectedMenuIndex += 2;
            requestUpdate();
          }
        }
      } else {
        if (bookCount > 0 && (leftPressed || rightPressed || upPressed || downPressed)) {
          const auto nav = ForkDriftNavigation::navigateCoverGrid(selectedBookIndex, bookCount, coverCols, coverRows,
                                                                  leftPressed, rightPressed, upPressed, downPressed);
          if (nav.enterButtonGrid) {
            inButtonGrid = true;
            selectedMenuIndex = 0;
          } else {
            selectedBookIndex = nav.bookIndex;
          }
          requestUpdate();
        }
      }
    } else {
      if (!recentBooks.empty()) {
        const int bookCount = static_cast<int>(recentBooks.size());
        if (leftPressed) {
          selectedBookIndex = (selectedBookIndex + bookCount - 1) % bookCount;
          requestUpdate();
        } else if (rightPressed) {
          selectedBookIndex = (selectedBookIndex + 1) % bookCount;
          requestUpdate();
        }
      }

      if (menuItemCount > 0) {
        if (upPressed) {
          selectedMenuIndex = (selectedMenuIndex + menuItemCount - 1) % menuItemCount;
          requestUpdate();
        } else if (downPressed) {
          selectedMenuIndex = (selectedMenuIndex + 1) % menuItemCount;
          requestUpdate();
        }
      }
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    const int continueIdx = hasContinueReading ? idx++ : -1;
    const int myLibraryIdx = idx++;
    const int opdsLibraryIdx =
        core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::OpdsBrowser, hasOpdsUrl) ? idx++ : -1;
    const int todoIdx =
        core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false) ? idx++ : -1;
    const int ankiIdx =
        core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false) ? idx++ : -1;
    const int notesIdx = idx++;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex == continueIdx) {
      onContinueReading();
    } else if (selectorIndex == myLibraryIdx) {
      onMyLibraryOpen();
    } else if (selectorIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (selectorIndex == todoIdx) {
      onTodoOpen();
    } else if (selectorIndex == ankiIdx) {
      onAnkiOpen();
    } else if (selectorIndex == notesIdx) {
      onNotesOpen();
    } else if (selectorIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (selectorIndex == settingsIdx) {
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
  // If we are using the new media picker UI, use its specialized rendering
  const bool mediaPickerEnabled = core::FeatureModules::hasCapability(core::Capability::HomeMediaPicker);
  if (mediaPickerEnabled) {
    const bool forkDrift = SETTINGS.uiTheme == CrossPointSettings::FORK_DRIFT;
    const int coverSelector = forkDrift && inButtonGrid ? -1 : selectedBookIndex;
    const int menuSelector = forkDrift && !inButtonGrid ? -1 : selectedMenuIndex;

    const int bookCountRender = static_cast<int>(recentBooks.size());
    const int singleRowH = metrics.homeCoverTileHeight / 2;
    const int coverTileH = forkDrift ? ((bookCountRender > 3 ? 2 : 1) * singleRowH) : metrics.homeCoverTileHeight;

    GUI.drawRecentBookCover(renderer, Rect(0, 0, pageWidth, coverTileH), recentBooks, coverSelector, coverRendered,
                            coverBufferStored, bufferRestored, [this]() { return storeCoverBuffer(); });

    std::vector<std::string> menuLabels;
    std::vector<UIIcon> menuIcons;
    menuLabels.reserve(6);
    menuIcons.reserve(6);

    if (forkDrift) {
      const bool pokemonPartyHomeMode = isPokemonPartyHomeMode();
      menuLabels.push_back(tr(STR_BOOKS));
      menuIcons.push_back(Folder);
      if (pokemonPartyHomeMode) {
        menuLabels.clear();
        menuIcons.clear();
        menuLabels.push_back(tr(STR_SETTINGS_TITLE));
        menuIcons.push_back(Settings);
      } else {
        if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false)) {
          menuLabels.push_back("Agenda");
          menuIcons.push_back(Text);
        }
        if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false)) {
          menuLabels.push_back("Anki");
          menuIcons.push_back(Text);  // Using Text icon as placeholder
        }
        menuLabels.push_back(tr(STR_FILE_TRANSFER));
        menuIcons.push_back(Transfer);
        menuLabels.push_back(tr(STR_SETTINGS_TITLE));
        menuIcons.push_back(Settings);
      }
    } else {
      menuLabels.push_back(recentBooks.empty() ? "Open Book (empty)" : "Open Book");
      menuIcons.push_back(Book);
      menuLabels.push_back("My Library");
      menuIcons.push_back(Folder);
      if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::OpdsBrowser, hasOpdsUrl)) {
        menuLabels.push_back("OPDS Browser");
        menuIcons.push_back(Library);
      }
      if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false)) {
        menuLabels.push_back("TODO");
        menuIcons.push_back(Text);
      }
      if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false)) {
        menuLabels.push_back("Anki");
        menuIcons.push_back(Text);
      }
      menuLabels.push_back("File Transfer");
      menuIcons.push_back(Transfer);
      menuLabels.push_back("Settings");
      menuIcons.push_back(Settings);
    }

    GUI.drawButtonMenu(
        renderer,
        Rect{0, coverTileH + metrics.verticalSpacing, pageWidth,
             pageHeight - (coverTileH + metrics.verticalSpacing * 2 + metrics.buttonHintsHeight)},
        static_cast<int>(menuLabels.size()), menuSelector, [&menuLabels](const int index) { return menuLabels[index]; },
        [&menuIcons](const int index) { return menuIcons[index]; });

    const char* backLabel = isPokemonPartyHomeMode() ? "Party" : "";
    const auto labels = mappedInput.mapLabels(backLabel, tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    constexpr int margin = 20;

    // --- Top "book" card for the current title (selectorIndex == 0) ---
    const int bookWidth = pageWidth / 2;
    const int bookHeight = pageHeight / 2;
    const int bookX = (pageWidth - bookWidth) / 2;
    constexpr int bookY = 30;
    const bool bookSelected = hasContinueReading && selectorIndex == 0;

    // Bookmark dimensions (used in multiple places)
    const int bookmarkWidth = bookWidth / 8;
    const int bookmarkHeight = bookHeight / 5;
    const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
    const int bookmarkY = bookY + 5;

    // Draw book card regardless, fill with message based on `hasContinueReading`
    {
      // Draw cover image as background if available (inside the box)
      // Only load from SD on first render, then use stored buffer
      if (hasContinueReading && hasCoverImage && !coverBmpPath.empty() && !coverRendered) {
        // First time: load cover from SD and render
        SpiBusMutex::Guard guard;
        FsFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            // Calculate position to center image within the book card
            int coverX, coverY;

            if (bitmap.getWidth() > bookWidth || bitmap.getHeight() > bookHeight) {
              const float imgRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
              const float boxRatio = static_cast<float>(bookWidth) / static_cast<float>(bookHeight);

              if (imgRatio > boxRatio) {
                coverX = bookX;
                coverY = bookY + (bookHeight - static_cast<int>(bookWidth / imgRatio)) / 2;
              } else {
                coverX = bookX + (bookWidth - static_cast<int>(bookHeight * imgRatio)) / 2;
                coverY = bookY;
              }
            } else {
              coverX = bookX + (bookWidth - bitmap.getWidth()) / 2;
              coverY = bookY + (bookHeight - bitmap.getHeight()) / 2;
            }

            // Draw the cover image centered within the book card
            renderer.drawBitmap(bitmap, coverX, coverY, bookWidth, bookHeight);

            // Draw border around the card
            renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

            // Store the buffer with cover image for fast navigation
            coverBufferStored = storeCoverBuffer();
            coverRendered = true;

            // First render: if selected, draw selection indicators now
            if (bookSelected) {
              renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
              renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
            }
          }
          file.close();
        }
      } else if (!bufferRestored && !coverRendered) {
        // No cover image: draw border or fill, plus bookmark as visual flair
        if (bookSelected) {
          renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
        } else {
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
        }

        // Draw bookmark ribbon when no cover image (visual decoration)
        if (hasContinueReading) {
          const int notchDepth = bookmarkHeight / 3;
          const int centerX = bookmarkX + bookmarkWidth / 2;

          const int xPoints[5] = {
              bookmarkX,                  // top-left
              bookmarkX + bookmarkWidth,  // top-right
              bookmarkX + bookmarkWidth,  // bottom-right
              centerX,                    // center notch point
              bookmarkX                   // bottom-left
          };
          const int yPoints[5] = {
              bookmarkY,                                // top-left
              bookmarkY,                                // top-right
              bookmarkY + bookmarkHeight,               // bottom-right
              bookmarkY + bookmarkHeight - notchDepth,  // center notch point
              bookmarkY + bookmarkHeight                // bottom-left
          };

          // Draw bookmark ribbon (inverted if selected)
          renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
        }
      }

      // If buffer was restored, draw selection indicators if needed
      if (bufferRestored && bookSelected && coverRendered) {
        // Draw selection border (no bookmark inversion needed since cover has no bookmark)
        renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
        renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
      }
    }

    if (hasContinueReading) {
      // Split into words
      std::vector<std::string> words;
      size_t pos = 0;
      while (pos < lastBookTitle.size()) {
        while (pos < lastBookTitle.size() && lastBookTitle[pos] == ' ') ++pos;
        if (pos >= lastBookTitle.size()) break;
        size_t start = pos;
        while (pos < lastBookTitle.size() && lastBookTitle[pos] != ' ') ++pos;
        words.push_back(lastBookTitle.substr(start, pos - start));
      }

      std::vector<std::string> lines;
      std::string currentLine;
      const int maxLineWidth = bookWidth - 40;
      const int spaceWidth = renderer.getSpaceWidth(UI_12_FONT_ID);

      for (auto& word : words) {
        if (lines.size() >= 3) {
          lines.back() += "...";
          break;
        }
        int wordWidth = renderer.getTextWidth(UI_12_FONT_ID, word.c_str());
        if (wordWidth > maxLineWidth) {
          while (renderer.getTextWidth(UI_12_FONT_ID, (word + "...").c_str()) > maxLineWidth && !word.empty()) {
            utf8RemoveLastChar(word);
          }
          word += "...";
        }

        int curWidth = renderer.getTextWidth(UI_12_FONT_ID, currentLine.c_str());
        if (!currentLine.empty() && curWidth + spaceWidth + wordWidth > maxLineWidth) {
          lines.push_back(currentLine);
          currentLine = word;
        } else {
          if (!currentLine.empty()) currentLine += " ";
          currentLine += word;
        }
      }
      if (!currentLine.empty() && lines.size() < 3) lines.push_back(currentLine);

      int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * lines.size();
      if (!lastBookAuthor.empty()) totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 1.5;

      int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

      if (coverRendered) {
        // Draw background box for text legibility over cover
        const int maxW =
            std::accumulate(lines.begin(), lines.end(), 0, [this](const int maxWidth, const std::string& line) {
              return std::max(maxWidth, renderer.getTextWidth(UI_12_FONT_ID, line.c_str()));
            });
        int boxW = maxW + 16;
        int boxH = totalTextHeight + 16;
        renderer.fillRect((pageWidth - boxW) / 2, titleYStart - 8, boxW, boxH, bookSelected);
        renderer.drawRect((pageWidth - boxW) / 2, titleYStart - 8, boxW, boxH, !bookSelected);
      }

      for (const auto& l : lines) {
        renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, l.c_str(), !bookSelected);
        titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
      }

      if (!lastBookAuthor.empty()) {
        titleYStart += renderer.getLineHeight(UI_10_FONT_ID) * 0.5;
        std::string author = lastBookAuthor;
        if (renderer.getTextWidth(UI_10_FONT_ID, author.c_str()) > maxLineWidth) {
          while (renderer.getTextWidth(UI_10_FONT_ID, (author + "...").c_str()) > maxLineWidth && !author.empty()) {
            utf8RemoveLastChar(author);
          }
          author += "...";
        }
        renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, author.c_str(), !bookSelected);
      }

      const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 1.5;
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, "Continue Reading", !bookSelected);
    } else {
      int y = bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    }

    // Draw other menu items
    int menuStartY = bookY + bookHeight + 30;
    int menuTileWidth = pageWidth - 40;
    int menuTileHeight = 45;
    int menuSpacing = 10;

    std::vector<const char*> labels_text = {"My Library"};
    if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::OpdsBrowser, hasOpdsUrl)) {
      labels_text.push_back("OPDS Browser");
    }
    if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false)) {
      labels_text.push_back("TODO");
    }
    if (core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::AnkiSupport, false)) {
      labels_text.push_back("Anki");
    }
    labels_text.push_back("File Transfer");
    labels_text.push_back("Settings");
    for (size_t i = 0; i < labels_text.size(); ++i) {
      int tileY = menuStartY + i * (menuTileHeight + menuSpacing);
      bool selected = (selectorIndex == (int)i + (hasContinueReading ? 1 : 0));
      if (selected)
        renderer.fillRect(20, tileY, menuTileWidth, menuTileHeight);
      else
        renderer.drawRect(20, tileY, menuTileWidth, menuTileHeight);
      renderer.drawCenteredText(UI_10_FONT_ID, tileY + (menuTileHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                                labels_text[i], !selected);
    }

    const auto hints = mappedInput.mapLabels("", "Select", "Up", "Down");
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  }

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onContinueReading() { activityManager.goToReader(APP_STATE.openEpubPath); }

void HomeActivity::onMyLibraryOpen() { activityManager.goToMyLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onTodoOpen() { activityManager.goToTodo(); }

void HomeActivity::onAnkiOpen() { activityManager.goToAnki(); }

void HomeActivity::onNotesOpen() { activityManager.goToNotes(); }
