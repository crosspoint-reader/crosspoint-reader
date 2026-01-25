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
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"
#include <ThemeManager.h>

void HomeActivity::taskTrampoline(void *param) {
  auto *self = static_cast<HomeActivity *>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 4; // Books, Files, Transfer, Settings
  if (hasOpdsUrl)
    count++; // + Calibre Library
  return count;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() &&
                       SdMan.exists(APP_STATE.openEpubPath.c_str());

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  if (hasContinueReading) {
    // Extract filename from path for display
    lastBookTitle = APP_STATE.openEpubPath;
    const size_t lastSlash = lastBookTitle.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookTitle = lastBookTitle.substr(lastSlash + 1);
    }

    // If epub, try to load the metadata for title/author and cover
    if (StringUtils::checkFileExtension(lastBookTitle, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        lastBookTitle = std::string(epub.getTitle());
      }
      if (!epub.getAuthor().empty()) {
        lastBookAuthor = std::string(epub.getAuthor());
      }
      // Try to generate thumbnail image for Continue Reading card
      if (epub.generateThumbBmp()) {
        coverBmpPath = epub.getThumbBmpPath();
        hasCoverImage = true;
      }
    } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtch") ||
               StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
      // Handle XTC file
      Xtc xtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) {
          lastBookTitle = std::string(xtc.getTitle());
        }
        if (!xtc.getAuthor().empty()) {
          lastBookAuthor = std::string(xtc.getAuthor());
        }
        // Try to generate thumbnail image for Continue Reading card
        if (xtc.generateThumbBmp()) {
          coverBmpPath = xtc.getThumbBmpPath();
          hasCoverImage = true;
        }
      }
      // Remove extension from title if we don't have metadata
      if (StringUtils::checkFileExtension(lastBookTitle, ".xtch")) {
        lastBookTitle.resize(lastBookTitle.length() - 5);
      } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
        lastBookTitle.resize(lastBookTitle.length() - 4);
      }
    }
  }

  selectorIndex = 0;
  lastBatteryCheck = 0; // Force update on first render
  coverRendered = false;
  coverBufferStored = false;

  // Load and cache recent books data (slow operation, do once)
  loadRecentBooksData();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              4096, // Stack size (increased for cover image rendering)
              this, // Parameters
              1,    // Priority
              &displayTaskHandle // Task handle
  );
}

void HomeActivity::loadRecentBooksData() {
  cachedRecentBooks.clear();
  
  const auto &recentBooks = RECENT_BOOKS.getBooks();
  const int maxRecentBooks = 3;
  int recentCount = std::min(static_cast<int>(recentBooks.size()), maxRecentBooks);
  
  for (int i = 0; i < recentCount; i++) {
    const std::string &bookPath = recentBooks[i];
    CachedBookInfo info;
    info.path = bookPath;  // Store the full path
    
    // Extract title from path
    info.title = bookPath;
    size_t lastSlash = info.title.find_last_of('/');
    if (lastSlash != std::string::npos) {
      info.title = info.title.substr(lastSlash + 1);
    }
    size_t lastDot = info.title.find_last_of('.');
    if (lastDot != std::string::npos) {
      info.title = info.title.substr(0, lastDot);
    }
    
    if (StringUtils::checkFileExtension(bookPath, ".epub")) {
      Epub epub(bookPath, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        info.title = epub.getTitle();
      }
      if (epub.generateThumbBmp()) {
        info.coverPath = epub.getThumbBmpPath();
      }
      
      // Read progress
      FsFile f;
      if (SdMan.openFileForRead("HOME", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          int spineIndex = data[0] + (data[1] << 8);
          int spineCount = epub.getSpineItemsCount();
          if (spineCount > 0) {
            info.progressPercent = (spineIndex * 100) / spineCount;
          }
        }
        f.close();
      }
    } else if (StringUtils::checkFileExtension(bookPath, ".xtc") ||
               StringUtils::checkFileExtension(bookPath, ".xtch")) {
      Xtc xtc(bookPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) {
          info.title = xtc.getTitle();
        }
        if (xtc.generateThumbBmp()) {
          info.coverPath = xtc.getThumbBmpPath();
        }
        
        // Read progress
        FsFile f;
        if (SdMan.openFileForRead("HOME", xtc.getCachePath() + "/progress.bin", f)) {
          uint8_t data[4];
          if (f.read(data, 4) == 4) {
            uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            uint32_t totalPages = xtc.getPageCount();
            if (totalPages > 0) {
              info.progressPercent = (currentPage * 100) / totalPages;
            }
          }
          f.close();
        }
      }
    }
    
    Serial.printf("[HOME] Book %d: title='%s', cover='%s', progress=%d%%\n", 
                  i, info.title.c_str(), info.coverPath.c_str(), info.progressPercent);
    cachedRecentBooks.push_back(info);
  }
  
  Serial.printf("[HOME] Loaded %d recent books\n", (int)cachedRecentBooks.size());
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to
  // EPD
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
  uint8_t *frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t *>(malloc(bufferSize));
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

  uint8_t *frameBuffer = renderer.getFrameBuffer();
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
  const bool upPressed = mappedInput.wasPressed(MappedInputManager::Button::Up);
  const bool downPressed = mappedInput.wasPressed(MappedInputManager::Button::Down);
  const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool rightPressed = mappedInput.wasPressed(MappedInputManager::Button::Right);
  const bool confirmPressed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  const int bookCount = static_cast<int>(cachedRecentBooks.size());
  const int menuCount = getMenuItemCount();
  const bool hasBooks = bookCount > 0;

  if (confirmPressed) {
    if (inBookSelection && hasBooks && bookSelectorIndex < bookCount) {
      // Open selected book - set the path and trigger continue reading
      APP_STATE.openEpubPath = cachedRecentBooks[bookSelectorIndex].path;
      onContinueReading();
      return;
    } else if (!inBookSelection) {
      // Menu selection - calculate which action
      int idx = 0;
      const int myLibraryIdx = idx++;
      const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
      const int filesIdx = idx++;
      const int transferIdx = idx++;
      const int settingsIdx = idx;

      if (selectorIndex == myLibraryIdx) {
        onMyLibraryOpen();
      } else if (selectorIndex == opdsLibraryIdx) {
        onOpdsBrowserOpen();
      } else if (selectorIndex == filesIdx) {
        onMyLibraryOpen(); // Files = file browser
      } else if (selectorIndex == transferIdx) {
        onFileTransferOpen(); // Transfer = web transfer
      } else if (selectorIndex == settingsIdx) {
        onSettingsOpen();
      }
    }
    return;
  }

  if (inBookSelection && hasBooks) {
    // Book selection mode
    if (leftPressed) {
      bookSelectorIndex = (bookSelectorIndex + bookCount - 1) % bookCount;
      updateRequired = true;
    } else if (rightPressed) {
      bookSelectorIndex = (bookSelectorIndex + 1) % bookCount;
      updateRequired = true;
    } else if (downPressed) {
      // Move to menu selection
      inBookSelection = false;
      selectorIndex = 0;
      updateRequired = true;
    }
  } else {
    // Menu selection mode
    if (upPressed) {
      if (selectorIndex == 0 && hasBooks) {
        // Move back to book selection
        inBookSelection = true;
        updateRequired = true;
      } else if (selectorIndex > 0) {
        selectorIndex--;
        updateRequired = true;
      }
    } else if (downPressed) {
      if (selectorIndex < menuCount - 1) {
        selectorIndex++;
        updateRequired = true;
      }
    } else if (leftPressed || rightPressed) {
      // In menu, left/right can also navigate (for 2-column layout)
      if (leftPressed && selectorIndex > 0) {
        selectorIndex--;
        updateRequired = true;
      } else if (rightPressed && selectorIndex < menuCount - 1) {
        selectorIndex++;
        updateRequired = true;
      }
    }
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
  // Battery check logic (only update every 60 seconds)
  const uint32_t now = millis();
  const bool needBatteryUpdate =
      (now - lastBatteryCheck > 60000) || (lastBatteryCheck == 0);
  if (needBatteryUpdate) {
    cachedBatteryLevel = battery.readPercentage();
    lastBatteryCheck = now;
  }

  // Always clear screen - ThemeEngine handles caching internally
  renderer.clearScreen();

  ThemeEngine::ThemeContext context;

  // --- Bind Global Data ---
  context.setString("BatteryPercent", std::to_string(cachedBatteryLevel));
  context.setBool("ShowBatteryPercent",
                  SETTINGS.hideBatteryPercentage !=
                      CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);

  // --- Recent Books Data (use cached data for performance) ---
  int recentCount = static_cast<int>(cachedRecentBooks.size());
  
  context.setBool("HasRecentBooks", recentCount > 0);
  context.setInt("RecentBooks.Count", recentCount);

  for (int i = 0; i < recentCount; i++) {
    const auto &book = cachedRecentBooks[i];
    std::string prefix = "RecentBooks." + std::to_string(i) + ".";
    
    context.setString(prefix + "Title", book.title);
    context.setString(prefix + "Image", book.coverPath);
    context.setString(prefix + "Progress", std::to_string(book.progressPercent));
    // Book is selected if we're in book selection mode and this is the selected index
    context.setBool(prefix + "Selected", inBookSelection && i == bookSelectorIndex);
  }

  // --- Book Card Data (for legacy theme) ---
  context.setBool("HasBook", hasContinueReading);
  context.setString("BookTitle", lastBookTitle);
  context.setString("BookAuthor", lastBookAuthor);
  context.setString("BookCoverPath", coverBmpPath);
  context.setBool("HasCover",
                  hasContinueReading && hasCoverImage && !coverBmpPath.empty());
  context.setBool("ShowInfoBox", true);

  // --- Selection Logic (for menu items, books handled separately) ---
  int idx = 0;
  const int myLibraryIdx = idx++;
  const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
  const int filesIdx = idx++;
  const int transferIdx = idx++;
  const int settingsIdx = idx;

  // IsBookSelected is true when we're in book selection mode
  context.setBool("IsBookSelected", inBookSelection);

  // --- Main Menu Data ---
  std::vector<std::string> menuLabels;
  std::vector<std::string> menuIcons;
  std::vector<bool> menuSelected;

  // Menu items are only selected when NOT in book selection mode
  const bool menuActive = !inBookSelection;

  menuLabels.push_back("Books");
  menuIcons.push_back("book");
  menuSelected.push_back(menuActive && selectorIndex == myLibraryIdx);

  if (hasOpdsUrl) {
    menuLabels.push_back("OPDS Browser");
    menuIcons.push_back("library");
    menuSelected.push_back(menuActive && selectorIndex == opdsLibraryIdx);
  }

  menuLabels.push_back("Files");
  menuIcons.push_back("folder");
  menuSelected.push_back(menuActive && selectorIndex == filesIdx);

  menuLabels.push_back("Transfer");
  menuIcons.push_back("transfer");
  menuSelected.push_back(menuActive && selectorIndex == transferIdx);

  menuLabels.push_back("Settings");
  menuIcons.push_back("settings");
  menuSelected.push_back(menuActive && selectorIndex == settingsIdx);

  context.setInt("MainMenu.Count", menuLabels.size());
  for (size_t i = 0; i < menuLabels.size(); ++i) {
    std::string prefix = "MainMenu." + std::to_string(i) + ".";
    context.setString(prefix + "Title", menuLabels[i]);
    context.setString(prefix + "Icon", menuIcons[i]);
    context.setBool(prefix + "Selected", menuSelected[i]);
  }

  // --- Render via ThemeEngine ---
  const uint32_t renderStart = millis();
  ThemeEngine::ThemeManager::get().renderScreen("Home", renderer, context);
  const uint32_t renderTime = millis() - renderStart;
  Serial.printf("[HOME] ThemeEngine render took %lums\n", renderTime);

  // After first full render, store the framebuffer for fast subsequent updates
  if (!coverRendered) {
    coverBufferStored = storeCoverBuffer();
    coverRendered = true;
  }

  const uint32_t displayStart = millis();
  renderer.displayBuffer();
  Serial.printf("[HOME] Display buffer took %lums\n", millis() - displayStart);
}
