#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // My Library, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks, int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  int progress = 0;
  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }

    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!SdMan.exists(coverPath.c_str())) {
        std::string lastBookFileName = "";
        const size_t lastSlash = book.path.find_last_of('/');
        if (lastSlash != std::string::npos) {
          lastBookFileName = book.path.substr(lastSlash + 1);
        }

        Serial.printf("Loading recent book: %s\n", book.path.c_str());

        // If epub, try to load the metadata for title/author and cover
        if (StringUtils::checkFileExtension(lastBookFileName, ".epub")) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, i18n(LOADING));
          }
          GUI.fillPopupProgress(renderer, popupRect, progress * 30);
          epub.generateThumbBmp(coverHeight);
        } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtch") ||
                   StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, i18n(LOADING));
            }
            GUI.fillPopupProgress(renderer, popupRect, progress * 30);
            xtc.generateThumbBmp(coverHeight);
          }
        }
      }
    }

    recentBooks.push_back(book);
    progress++;
  }

  Serial.printf("Recent books loaded: %d\n", recentBooks.size());
  recentsLoaded = true;
  recentsLoading = false;
  updateRequired = true;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              8192,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  // Free the stored cover buffer if any
  freeCoverBuffer();
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
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int myLibraryIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == myLibraryIdx) {
      onMyLibraryOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  if (!firstRenderDone || (recentsLoaded && !recentsDisplayed)) {
    renderer.clearScreen();
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  if (hasContinueReading) {
    if (recentsLoaded) {
      recentsDisplayed = true;
      GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                              recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                              std::bind(&HomeActivity::storeCoverBuffer, this));
    } else if (!recentsLoading && firstRenderDone) {
      recentsLoading = true;
      loadRecentBooks(metrics.homeRecentBooksCount, metrics.homeCoverHeight);
    }
  }

  // Build menu items dynamically
  std::vector<const char*> menuItems = {i18n(BROWSE_FILES), i18n(MENU_RECENT_BOOKS), i18n(FILE_TRANSFER), i18n(SETTINGS_TITLE)};
  if (hasOpdsUrl) {
    // Insert OPDS Browser after My Library
    menuItems.insert(menuItems.begin() + 2, i18n(OPDS_BROWSER));
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); }, nullptr);

  const auto labels = mappedInput.mapLabels("", i18n(SELECT), i18n(DIR_UP), i18n(DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    updateRequired = true;
  }
}
