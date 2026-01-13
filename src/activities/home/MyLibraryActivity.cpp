#include "MyLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ScreenComponents.h"
#include "fontIds.h"

namespace {
// Layout constants
constexpr int TAB_BAR_Y = 15;
constexpr int CONTENT_START_Y = 60;
constexpr int LINE_HEIGHT = 30;
constexpr int LEFT_MARGIN = 20;
constexpr int RIGHT_MARGIN = 40; // Extra space for scroll indicator

// Timing thresholds
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;

void sortFileList(std::vector<std::string> &strs) {
  std::sort(begin(strs), end(strs),
            [](const std::string &str1, const std::string &str2) {
              if (str1.back() == '/' && str2.back() != '/')
                return true;
              if (str1.back() != '/' && str2.back() == '/')
                return false;
              return lexicographical_compare(
                  begin(str1), end(str1), begin(str2), end(str2),
                  [](const char &char1, const char &char2) {
                    return tolower(char1) < tolower(char2);
                  });
            });
}
} // namespace

int MyLibraryActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int bottomBarHeight = 60; // Space for button hints
  const int availableHeight = screenHeight - CONTENT_START_Y - bottomBarHeight;
  int items = availableHeight / LINE_HEIGHT;
  if (items < 1) {
    items = 1;
  }
  return items;
}

int MyLibraryActivity::getCurrentItemCount() const {
  if (currentTab == Tab::Recent) {
    return static_cast<int>(bookTitles.size());
  }
  return static_cast<int>(files.size());
}

int MyLibraryActivity::getTotalPages() const {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();
  if (itemCount == 0)
    return 1;
  return (itemCount + pageItems - 1) / pageItems;
}

int MyLibraryActivity::getCurrentPage() const {
  const int pageItems = getPageItems();
  return selectorIndex / pageItems + 1;
}

void MyLibraryActivity::loadRecentBooks() {
  constexpr size_t MAX_RECENT_BOOKS = 20;

  bookTitles.clear();
  bookPaths.clear();
  const auto &books = RECENT_BOOKS.getBooks();
  bookTitles.reserve(std::min(books.size(), MAX_RECENT_BOOKS));
  bookPaths.reserve(std::min(books.size(), MAX_RECENT_BOOKS));

  for (const auto &path : books) {
    // Limit to maximum number of recent books
    if (bookTitles.size() >= MAX_RECENT_BOOKS) {
      break;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(path.c_str())) {
      continue;
    }

    // Extract filename from path for display
    std::string title = path;
    const size_t lastSlash = title.find_last_of('/');
    if (lastSlash != std::string::npos) {
      title = title.substr(lastSlash + 1);
    }

    const std::string ext5 =
        title.length() >= 5 ? title.substr(title.length() - 5) : "";
    const std::string ext4 =
        title.length() >= 4 ? title.substr(title.length() - 4) : "";

    // If epub, try to load the metadata for title
    if (ext5 == ".epub") {
      Epub epub(path, "/.crosspoint");
      epub.load(false);
      if (!epub.getTitle().empty()) {
        title = std::string(epub.getTitle());
      }
    } else if (ext5 == ".xtch") {
      title.resize(title.length() - 5);
    } else if (ext4 == ".xtc") {
      title.resize(title.length() - 4);
    }

    bookTitles.push_back(title);
    bookPaths.push_back(path);
  }
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root)
      root.close();
    return;
  }

  root.rewindDirectory();

  char name[128];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      std::string ext4 =
          filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
      std::string ext5 =
          filename.length() >= 5 ? filename.substr(filename.length() - 5) : "";
      if (ext5 == ".epub" || ext5 == ".xtch" || ext4 == ".xtc") {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void MyLibraryActivity::taskTrampoline(void *param) {
  auto *self = static_cast<MyLibraryActivity *>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load data for both tabs
  loadRecentBooks();
  loadFiles();

  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              4096, // Stack size (increased for epub metadata loading)
              this, // Parameters
              1,    // Priority
              &displayTaskHandle // Task handle
  );
}

void MyLibraryActivity::onExit() {
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

  bookTitles.clear();
  bookPaths.clear();
  files.clear();
}

void MyLibraryActivity::loop() {
  const int itemCount = getCurrentItemCount();
  const int pageItems = getPageItems();

  // Long press BACK (1s+) in Files tab goes to root folder
  if (currentTab == Tab::Files &&
      mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    }
    return;
  }

  const bool upReleased =
      mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased =
      mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased =
      mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased =
      mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  // Confirm button - open selected item
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (currentTab == Tab::Recent) {
      if (!bookPaths.empty() &&
          selectorIndex < static_cast<int>(bookPaths.size())) {
        onSelectBook(bookPaths[selectorIndex]);
      }
    } else {
      // Files tab
      if (!files.empty() && selectorIndex < static_cast<int>(files.size())) {
        if (basepath.back() != '/')
          basepath += "/";
        if (files[selectorIndex].back() == '/') {
          // Enter directory
          basepath +=
              files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
          loadFiles();
          selectorIndex = 0;
          updateRequired = true;
        } else {
          // Open file
          onSelectBook(basepath + files[selectorIndex]);
        }
      }
    }
    return;
  }

  // Back button
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (currentTab == Tab::Files && basepath != "/") {
        // Go up one directory
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty())
          basepath = "/";
        loadFiles();
        selectorIndex = 0;
        updateRequired = true;
      } else {
        // Go home
        onGoHome();
      }
    }
    return;
  }

  // Tab switching: Left/Right always control tabs
  if (leftReleased && currentTab == Tab::Files) {
    currentTab = Tab::Recent;
    selectorIndex = 0;
    updateRequired = true;
    return;
  }
  if (rightReleased && currentTab == Tab::Recent) {
    currentTab = Tab::Files;
    selectorIndex = 0;
    updateRequired = true;
    return;
  }

  // Navigation: Up/Down moves through items only
  const bool prevReleased = upReleased;
  const bool nextReleased = downReleased;

  if (prevReleased && itemCount > 0) {
    if (skipPage) {
      selectorIndex =
          ((selectorIndex / pageItems - 1) * pageItems + itemCount) % itemCount;
    } else {
      selectorIndex = (selectorIndex + itemCount - 1) % itemCount;
    }
    updateRequired = true;
  } else if (nextReleased && itemCount > 0) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % itemCount;
    } else {
      selectorIndex = (selectorIndex + 1) % itemCount;
    }
    updateRequired = true;
  }
}

void MyLibraryActivity::displayTaskLoop() {
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

void MyLibraryActivity::render() const {
  renderer.clearScreen();

  // Draw tab bar
  std::vector<TabInfo> tabs = {{"Recent", currentTab == Tab::Recent},
                               {"Files", currentTab == Tab::Files}};
  ScreenComponents::drawTabBar(renderer, TAB_BAR_Y, tabs);

  // Draw content based on current tab
  if (currentTab == Tab::Recent) {
    renderRecentTab();
  } else {
    renderFilesTab();
  }

  // Draw scroll indicator
  const int screenHeight = renderer.getScreenHeight();
  const int contentHeight =
      screenHeight - CONTENT_START_Y - 60; // 60 for bottom bar
  ScreenComponents::drawScrollIndicator(renderer, getCurrentPage(),
                                        getTotalPages(), CONTENT_START_Y,
                                        contentHeight);

  // Draw side button hints (up/down navigation on right side)
  // Note: text is rotated 90° CW, so ">" appears as "^" and "<" appears as "v"
  renderer.drawSideButtonHints(UI_10_FONT_ID, ">", "<");

  // Draw bottom button hints
  // In Files tab, show "BACK" when in subdirectory, "HOME" when at root
  const char *backLabel =
      (currentTab == Tab::Files && basepath != "/") ? "BACK" : "HOME";
  const auto labels = mappedInput.mapLabels(backLabel, "OPEN", "<", ">");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3,
                           labels.btn4);

  renderer.displayBuffer();
}

void MyLibraryActivity::renderRecentTab() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int bookCount = static_cast<int>(bookTitles.size());

  if (bookCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y,
                      "No recent books");
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Draw selection highlight
  renderer.fillRect(
      0, CONTENT_START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2,
      pageWidth - RIGHT_MARGIN, LINE_HEIGHT);

  // Draw items
  for (int i = pageStartIndex; i < bookCount && i < pageStartIndex + pageItems;
       i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, bookTitles[i].c_str(),
                                       pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN,
                      CONTENT_START_Y + (i % pageItems) * LINE_HEIGHT,
                      item.c_str(), i != selectorIndex);
  }
}

void MyLibraryActivity::renderFilesTab() const {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int fileCount = static_cast<int>(files.size());

  if (fileCount == 0) {
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN, CONTENT_START_Y,
                      "No books found");
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;

  // Draw selection highlight
  renderer.fillRect(
      0, CONTENT_START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2,
      pageWidth - RIGHT_MARGIN, LINE_HEIGHT);

  // Draw items
  for (int i = pageStartIndex; i < fileCount && i < pageStartIndex + pageItems;
       i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(),
                                       pageWidth - LEFT_MARGIN - RIGHT_MARGIN);
    renderer.drawText(UI_10_FONT_ID, LEFT_MARGIN,
                      CONTENT_START_Y + (i % pageItems) * LINE_HEIGHT,
                      item.c_str(), i != selectorIndex);
  }
}
