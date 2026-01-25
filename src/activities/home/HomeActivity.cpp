#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <ThemeManager.h>
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

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 3;            // Browse Files, File Transfer, Settings
  if (hasOpdsUrl) count++;  // + Calibre Library
  return count;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Reset render and selection state
  coverRendered = false;
  coverBufferStored = false;
  freeCoverBuffer();
  selectorIndex = 0;  // Start at first item (first book if any, else first menu)

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

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
  lastBatteryCheck = 0;  // Force update on first render
  coverRendered = false;
  coverBufferStored = false;

  // Load and cache recent books data (slow operation, do once)
  loadRecentBooksData();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              4096,               // Stack size (increased for cover image rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::loadRecentBooksData() {
  cachedRecentBooks.clear();

  const auto& recentBooks = RECENT_BOOKS.getBooks();
  const int maxRecentBooks = 3;
  int recentCount = std::min(static_cast<int>(recentBooks.size()), maxRecentBooks);

  for (int i = 0; i < recentCount; i++) {
    const std::string& bookPath = recentBooks[i];
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

    Serial.printf("[HOME] Book %d: title='%s', cover='%s', progress=%d%%\n", i, info.title.c_str(),
                  info.coverPath.c_str(), info.progressPercent);
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
  const bool confirmPressed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  // Navigation uses theme-configured book slots (limited by actual books available)
  const int maxBooks = static_cast<int>(cachedRecentBooks.size());
  const int themeBookCount = ThemeEngine::ThemeManager::get().getNavBookCount();
  const int navBookCount = std::min(themeBookCount, maxBooks);
  const int menuCount = getMenuItemCount();
  const int totalCount = navBookCount + menuCount;

  if (confirmPressed) {
    if (selectorIndex < navBookCount && selectorIndex < maxBooks) {
      // Book selected - open the selected book
      APP_STATE.openEpubPath = cachedRecentBooks[selectorIndex].path;
      onContinueReading();
    } else {
      // Menu item selected
      const int menuIdx = selectorIndex - navBookCount;
      int idx = 0;
      const int myLibraryIdx = idx++;
      const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
      const int fileTransferIdx = idx++;
      const int settingsIdx = idx;

      if (menuIdx == myLibraryIdx) {
        onMyLibraryOpen();
      } else if (menuIdx == opdsLibraryIdx) {
        onOpdsBrowserOpen();
      } else if (menuIdx == fileTransferIdx) {
        onFileTransferOpen();
      } else if (menuIdx == settingsIdx) {
        onSettingsOpen();
      }
    }
    return;
  }

  if (prevPressed) {
    selectorIndex = (selectorIndex + totalCount - 1) % totalCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % totalCount;
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
  // Battery check logic (only update every 60 seconds)
  const uint32_t now = millis();
  const bool needBatteryUpdate = (now - lastBatteryCheck > 60000) || (lastBatteryCheck == 0);
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
                  SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);

  // --- Navigation counts (must match loop()) ---
  const int recentCount = static_cast<int>(cachedRecentBooks.size());
  const int themeBookCount = ThemeEngine::ThemeManager::get().getNavBookCount();
  const int navBookCount = std::min(themeBookCount, recentCount);
  const bool isBookSelected = selectorIndex < navBookCount;

  // --- Recent Books Data ---
  context.setBool("HasRecentBooks", recentCount > 0);
  context.setInt("RecentBooks.Count", recentCount);
  context.setInt("SelectedBookIndex", isBookSelected ? selectorIndex : -1);

  for (int i = 0; i < recentCount; i++) {
    const auto& book = cachedRecentBooks[i];
    std::string prefix = "RecentBooks." + std::to_string(i) + ".";

    context.setString(prefix + "Title", book.title);
    context.setString(prefix + "Image", book.coverPath);
    context.setString(prefix + "Progress", std::to_string(book.progressPercent));
    // Book is selected if selectorIndex matches
    context.setBool(prefix + "Selected", selectorIndex == i);
  }

  // --- Book Card Data (for themes with single book) ---
  context.setBool("IsBookSelected", isBookSelected);
  context.setBool("HasBook", hasContinueReading);
  context.setString("BookTitle", lastBookTitle);
  context.setString("BookAuthor", lastBookAuthor);
  context.setString("BookCoverPath", coverBmpPath);
  context.setBool("HasCover", hasContinueReading && hasCoverImage && !coverBmpPath.empty());
  context.setBool("ShowInfoBox", true);

  // Default values
  std::string chapterTitle = "";
  std::string currentPageStr = "-";
  std::string totalPagesStr = "-";
  int progressPercent = 0;

  if (hasContinueReading) {
    if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);

      // Read progress
      FsFile f;
      if (SdMan.openFileForRead("HOME", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          int spineIndex = data[0] + (data[1] << 8);
          int spineCount = epub.getSpineItemsCount();

          currentPageStr = std::to_string(spineIndex + 1);  // Display 1-based
          totalPagesStr = std::to_string(spineCount);

          if (spineCount > 0) {
            progressPercent = (spineIndex * 100) / spineCount;
          }

          // Resolve Chapter Title
          auto spineEntry = epub.getSpineItem(spineIndex);
          if (spineEntry.tocIndex != -1) {
            auto tocEntry = epub.getTocItem(spineEntry.tocIndex);
            chapterTitle = tocEntry.title;
          }
        }
        f.close();
      }
    } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
               StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
      Xtc xtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (xtc.load()) {
        // Read progress
        FsFile f;
        if (SdMan.openFileForRead("HOME", xtc.getCachePath() + "/progress.bin", f)) {
          uint8_t data[4];
          if (f.read(data, 4) == 4) {
            uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            uint32_t totalPages = xtc.getPageCount();

            currentPageStr = std::to_string(currentPage + 1);  // 1-based
            totalPagesStr = std::to_string(totalPages);

            if (totalPages > 0) {
              progressPercent = (currentPage * 100) / totalPages;
            }

            chapterTitle = "Page " + currentPageStr;
          }
          f.close();
        }
      }
    }
  }

  context.setString("BookChapter", chapterTitle);
  context.setString("BookCurrentPage", currentPageStr);
  context.setString("BookTotalPages", totalPagesStr);
  context.setInt("BookProgressPercent", progressPercent);
  context.setString("BookProgressPercentStr", std::to_string(progressPercent));

  // --- Main Menu Data ---
  // Menu items start after the book slot
  const int menuStartIdx = navBookCount;

  int idx = 0;
  const int myLibraryIdx = menuStartIdx + idx++;
  const int opdsLibraryIdx = hasOpdsUrl ? menuStartIdx + idx++ : -1;
  const int fileTransferIdx = menuStartIdx + idx++;
  const int settingsIdx = menuStartIdx + idx;

  std::vector<std::string> menuLabels;
  std::vector<std::string> menuIcons;
  std::vector<bool> menuSelected;

  menuLabels.push_back("Browse Files");
  menuIcons.push_back("folder");
  menuSelected.push_back(selectorIndex == myLibraryIdx);

  if (hasOpdsUrl) {
    menuLabels.push_back("OPDS Browser");
    menuIcons.push_back("library");
    menuSelected.push_back(selectorIndex == opdsLibraryIdx);
  }

  menuLabels.push_back("File Transfer");
  menuIcons.push_back("transfer");
  menuSelected.push_back(selectorIndex == fileTransferIdx);

  menuLabels.push_back("Settings");
  menuIcons.push_back("settings");
  menuSelected.push_back(selectorIndex == settingsIdx);

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
