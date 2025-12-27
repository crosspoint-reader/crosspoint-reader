#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <InputManager.h>
#include <SD.h>

#include "config.h"

namespace {
constexpr int PAGE_ITEMS = 23;
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

void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

void FileSelectionActivity::loadFiles() {
  files.clear();
  selectorIndex = 0;
  auto root = SD.open(basepath.c_str());
  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    auto filename = std::string(file.name());
    if (filename[0] == '.') {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(filename + "/");
    } else if (filename.substr(filename.length() - 5) == ".epub") {
      files.emplace_back(filename);
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // basepath is set via constructor parameter (defaults to "/" if not specified)
  loadFiles();
  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FileSelectionActivity::onExit() {
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

void FileSelectionActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (inputManager.isPressed(InputManager::BTN_BACK) && inputManager.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool prevReleased =
      inputManager.wasReleased(InputManager::BTN_UP) || inputManager.wasReleased(InputManager::BTN_LEFT);
  const bool nextReleased =
      inputManager.wasReleased(InputManager::BTN_DOWN) || inputManager.wasReleased(InputManager::BTN_RIGHT);

  const bool skipPage = inputManager.getHeldTime() > SKIP_PAGE_MS;

  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      loadFiles();
      updateRequired = true;
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
  } else if (inputManager.wasReleased(InputManager::BTN_BACK)) {
    // Short press: go up one directory, or go home if at root
    if (inputManager.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }
}

void FileSelectionActivity::displayTaskLoop() {
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

void FileSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = GfxRenderer::getScreenWidth();
  renderer.drawCenteredText(READER_FONT_ID, 10, "Books", true, BOLD);

  // Help text
  renderer.drawButtonHints(UI_FONT_ID, "« Home", "Open", "", "");

  if (files.empty()) {
    renderer.drawText(UI_FONT_ID, 20, 60, "No EPUBs found");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 + 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = files[i];
    int itemWidth = renderer.getTextWidth(UI_FONT_ID, item.c_str());
    while (itemWidth > renderer.getScreenWidth() - 40 && item.length() > 8) {
      item.replace(item.length() - 5, 5, "...");
      itemWidth = renderer.getTextWidth(UI_FONT_ID, item.c_str());
    }
    renderer.drawText(UI_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }

  renderer.displayBuffer();
}
