#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
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
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
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
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  files.clear();
}

void MyLibraryActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (currentTab == Tab::Files && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    }
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (currentTab == Tab::Recent) {
      if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
        Serial.printf("Selected recent book: %s\n", recentBooks[selectorIndex].path.c_str());
        onSelectBook(recentBooks[selectorIndex].path, currentTab);
        return;
      }
    } else {
      if (files.empty()) {
        return;
      }

      if (basepath.back() != '/') basepath += "/";
      if (files[selectorIndex].back() == '/') {
        basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
        loadFiles();
        selectorIndex = 0;
        updateRequired = true;
      } else {
        onSelectBook(basepath + files[selectorIndex], currentTab);
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (currentTab == Tab::Files && basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  }

  // Tab switching: Left/Right always control tabs
  if (leftReleased || rightReleased) {
    if (currentTab == Tab::Files) {
      currentTab = Tab::Recent;
    } else {
      currentTab = Tab::Files;
    }
    selectorIndex = 0;
    updateRequired = true;
    return;
  }

  int listSize = (currentTab == Tab::Recent) ? static_cast<int>(recentBooks.size()) : static_cast<int>(files.size());
  if (upReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + listSize) % listSize;
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (downReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % listSize;
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getMetrics();

  auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
  UITheme::drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);

  UITheme::drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      {{"Recent", currentTab == Tab::Recent}, {"Files", currentTab == Tab::Files}});

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  if (currentTab == Tab::Recent) {
    // Recent tab
    if (recentBooks.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No recent books");
    } else {
      UITheme::drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
          [this](int index) { return recentBooks[index].title; }, false, nullptr, false, nullptr);
    }
  } else {
    if (files.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No books found");
    } else {
      UITheme::drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
          [this](int index) { return files[index]; }, false, nullptr, false, nullptr);
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "Up", "Down");
  UITheme::drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  UITheme::drawSideButtonHints(renderer, "^", "v");

  renderer.displayBuffer();
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
