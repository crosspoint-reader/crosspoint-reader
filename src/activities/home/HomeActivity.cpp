#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
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
  int count = 3;  // My Library, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks, int coverHeight, PopupCallbacks& popupCallbacks) {
  recentsLoading = true;

  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  int progress = 0;
  bool loadingPopupDisplayed = false;
  for (const RecentBook& book : books) {
    const std::string& path = book.path;

    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(path.c_str())) {
      continue;
    }

    std::string coverBmpPath = "";
    std::string lastBookFileName = "";
    const size_t lastSlash = path.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookFileName = path.substr(lastSlash + 1);
    }

    Serial.printf("Loading recent book: %s\n", path.c_str());

    // If epub, try to load the metadata for title/author and cover
    if (StringUtils::checkFileExtension(lastBookFileName, ".epub")) {
      Epub epub(path, "/.crosspoint");
      epub.load(false);
      // if (!epub.getTitle().empty()) {
      //   lastBookTitle = std::string(epub.getTitle());
      // }
      // if (!epub.getAuthor().empty()) {
      //   lastBookAuthor = std::string(epub.getAuthor());
      // }
      // Try to generate thumbnail image for Continue Reading card
      coverBmpPath = epub.getThumbBmpPath(coverHeight);
      if (!SdMan.exists(coverBmpPath.c_str())) {
        if (loadingPopupDisplayed) {
          popupCallbacks.update(progress * 30);
        } else {
          popupCallbacks.setup();
          loadingPopupDisplayed = true;
        }
        if (!epub.generateThumbBmp(coverHeight)) {
          coverBmpPath = "";
        }
      }
    } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtch") ||
               StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
      // Handle XTC file
      Xtc xtc(path, "/.crosspoint");
      if (xtc.load()) {
        // if (!xtc.getTitle().empty()) {
        //   lastBookTitle = std::string(xtc.getTitle());
        // }
        // Try to generate thumbnail image for Continue Reading card
        coverBmpPath = xtc.getThumbBmpPath(coverHeight);
        if (!SdMan.exists(coverBmpPath.c_str())) {
          if (loadingPopupDisplayed) {
            popupCallbacks.update(progress * 30);
          } else {
            popupCallbacks.setup();
            loadingPopupDisplayed = true;
          }
          if (!xtc.generateThumbBmp(coverHeight)) {
            coverBmpPath = "";
          }
        }
      }

      // if (lastBookTitle.empty()) {
      //   // Remove extension from title if we don't have metadata
      //   if (StringUtils::checkFileExtension(lastBookFileName, ".xtch")) {
      //     lastBookFileName.resize(lastBookFileName.length() - 5);
      //   } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
      //     lastBookFileName.resize(lastBookFileName.length() - 4);
      //   }
      //   lastBookTitle = lastBookFileName;
      // }
    }

    recentBooks.push_back(RecentBookWithCover{book, coverBmpPath});
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
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].book.path, MyLibraryActivity::Tab::Recent);
    } else if (menuSelectedIndex == myLibraryIdx) {
      onMyLibraryOpen();
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
  auto metrics = UITheme::getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  if (!firstRenderDone || (recentsLoaded && !recentsDisplayed)) {
    renderer.clearScreen();
  }

  UITheme::drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  if (hasContinueReading) {
    if (recentsLoaded) {
      recentsDisplayed = true;
      UITheme::drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverHeight},
                                   recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                                   std::bind(&HomeActivity::storeCoverBuffer, this));
    } else if (!recentsLoading && firstRenderDone) {
      recentsLoading = true;
      PopupCallbacks popupCallbacks = UITheme::drawPopupWithProgress(renderer, "Loading...");
      loadRecentBooks(metrics.homeRecentBooksCount, metrics.homeCoverHeight, popupCallbacks);
    }
  }

  // Build menu items dynamically
  std::vector<const char*> menuItems = {"Browse Files", "File Transfer", "Settings"};
  if (hasOpdsUrl) {
    // Insert OPDS Browser after My Library
    menuItems.insert(menuItems.begin() + 1, "OPDS Browser");
  }

  UITheme::drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); }, false, nullptr);

  const auto labels = mappedInput.mapLabels("", "Select", "Up", "Down");
  UITheme::drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    updateRequired = true;
  }
}
