#include "MyLibraryActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long DELETE_CONFIRM_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
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
  ActivityWithSubactivity::onExit();

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
  // Delegate to subactivity (e.g. keyboard for rename)
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Delete confirmation state
  if (state == State::DELETE_CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      deleteSelectedItem();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = State::BROWSING;
      deleteError.clear();
      updateRequired = true;
    }
    return;
  }

  // Delete menu state (side button overlay)
  if (state == State::DELETE_MENU) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      // Volume up = Delete → go to delete confirmation
      state = State::DELETE_CONFIRM;
      deleteError.clear();
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      // Volume down = Rename → open keyboard
      startRename();
      return;
    }
    // Any other press cancels the menu
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::BROWSING;
      skipNextConfirmRelease = true;
      updateRequired = true;
      return;
    }
    return;
  }

  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    updateRequired = true;
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Long press CONFIRM (1s+) opens the delete menu with side button options
  if (!files.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= DELETE_CONFIRM_MS) {
    state = State::DELETE_MENU;
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (skipNextConfirmRelease) {
      skipNextConfirmRelease = false;
      return;
    }
    if (files.empty()) {
      return;
    }

    // Only open on short press (long press already handled above)
    if (mappedInput.getHeldTime() < DELETE_CONFIRM_MS) {
      if (basepath.back() != '/') basepath += "/";
      if (files[selectorIndex].back() == '/') {
        basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
        loadFiles();
        selectorIndex = 0;
        updateRequired = true;
      } else {
        onSelectBook(basepath + files[selectorIndex]);
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
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

  int listSize = static_cast<int>(files.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    updateRequired = true;
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    updateRequired = true;
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    updateRequired = true;
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    updateRequired = true;
  });
}

void MyLibraryActivity::deleteSelectedItem() {
  if (selectorIndex >= files.size()) return;

  std::string itemName = files[selectorIndex];
  const bool isDir = !itemName.empty() && itemName.back() == '/';
  if (isDir) itemName = itemName.substr(0, itemName.length() - 1);

  std::string fullPath = basepath;
  if (fullPath.back() != '/') fullPath += "/";
  fullPath += itemName;

  Serial.printf("[%lu] [MY_LIBRARY] Deleting: %s\n", millis(), fullPath.c_str());

  bool success;
  if (isDir) {
    success = Storage.rmdir(fullPath.c_str());
  } else {
    success = Storage.remove(fullPath.c_str());
  }

  if (success) {
    Serial.printf("[%lu] [MY_LIBRARY] Deleted successfully: %s\n", millis(), fullPath.c_str());

    if (!isDir) {
      RECENT_BOOKS.removeBook(fullPath);
    }

    loadFiles();
    if (selectorIndex >= files.size() && !files.empty()) {
      selectorIndex = files.size() - 1;
    }
    state = State::BROWSING;
    deleteError.clear();
    skipNextConfirmRelease = true;
  } else {
    Serial.printf("[%lu] [MY_LIBRARY] Failed to delete: %s\n", millis(), fullPath.c_str());
    deleteError = isDir ? "Folder must be empty" : "Failed to delete file";
  }

  updateRequired = true;
}

void MyLibraryActivity::startRename() {
  if (selectorIndex >= files.size()) return;

  std::string itemName = files[selectorIndex];
  const bool isDir = !itemName.empty() && itemName.back() == '/';
  if (isDir) itemName = itemName.substr(0, itemName.length() - 1);

  // For files, strip the extension so the user only edits the name
  std::string extension;
  if (!isDir) {
    const auto dotPos = itemName.rfind('.');
    if (dotPos != std::string::npos) {
      extension = itemName.substr(dotPos);
      itemName = itemName.substr(0, dotPos);
    }
  }

  state = State::BROWSING;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, "Rename", itemName, 10,
      0,      // unlimited length
      false,  // not password
      [this, isDir, extension](const std::string& newName) {
        if (!newName.empty() && selectorIndex < files.size()) {
          std::string dir = basepath;
          if (dir.back() != '/') dir += "/";

          std::string oldItemName = files[selectorIndex];
          if (isDir) oldItemName = oldItemName.substr(0, oldItemName.length() - 1);

          const std::string oldPath = dir + oldItemName;
          const std::string newFileName = newName + extension;
          const std::string newPath = dir + (isDir ? newName : newFileName);

          Serial.printf("[%lu] [MY_LIBRARY] Renaming: %s -> %s\n", millis(), oldPath.c_str(), newPath.c_str());

          if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
            Serial.printf("[%lu] [MY_LIBRARY] Renamed successfully\n", millis());

            if (!isDir) {
              // Update recent books store if this book was tracked
              const auto bookData = RECENT_BOOKS.getDataFromBook(oldPath);
              if (!bookData.path.empty()) {
                RECENT_BOOKS.removeBook(oldPath);
                RECENT_BOOKS.addBook(newPath, bookData.title, bookData.author, bookData.coverBmpPath);
              }
            }

            loadFiles();
            // Select the renamed item
            selectorIndex = findEntry(isDir ? newName + "/" : newFileName);
          } else {
            Serial.printf("[%lu] [MY_LIBRARY] Failed to rename\n", millis());
          }
        }
        exitActivity();
        updateRequired = true;
      },
      [this]() {
        exitActivity();
        updateRequired = true;
      }));
  xSemaphoreGive(renderingMutex);
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
  auto metrics = UITheme::getInstance().getMetrics();

  if (state == State::DELETE_CONFIRM && selectorIndex < files.size()) {
    std::string itemName = files[selectorIndex];
    const bool isDir = !itemName.empty() && itemName.back() == '/';
    if (isDir) itemName = itemName.substr(0, itemName.length() - 1);

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Delete Item");

    if (deleteError.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Are you sure you want to delete:", true);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, itemName.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, "This action cannot be undone!", true);

      const auto labels = mappedInput.mapLabels("« Cancel", "Delete", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, deleteError.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, itemName.c_str(), true);

      const auto labels = mappedInput.mapLabels("« Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

    renderer.displayBuffer();
    return;
  }

  auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No books found");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return files[index]; }, nullptr, nullptr, nullptr);
  }

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Draw side button menu overlay when in DELETE_MENU state
  if (state == State::DELETE_MENU) {
    GUI.drawSideButtonHints(renderer, "Delete", "Rename");
  }

  renderer.displayBuffer();
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}