#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SD.h>

#include "config.h"

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
  renderingMutex = xSemaphoreCreateMutex();

  basepath = "/";
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
  const bool prevPressed =
      inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT);
  const bool nextPressed =
      inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT);

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
  } else if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    if (basepath != "/") {
      basepath = basepath.substr(0, basepath.rfind('/'));
      if (basepath.empty()) basepath = "/";
      loadFiles();
      updateRequired = true;
    } else {
      // At root level, go back home
      onGoHome();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % files.size();
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
  renderer.drawCenteredText(READER_FONT_ID, 10, "CrossPoint Reader", true, BOLD);

  // Help text
  renderer.drawText(SMALL_FONT_ID, 20, GfxRenderer::getScreenHeight() - 30, "Press BACK for Home");

  if (files.empty()) {
    renderer.drawText(UI_FONT_ID, 20, 60, "No EPUBs found");
  } else {
    // Draw selection
    renderer.fillRect(0, 60 + selectorIndex * 30 + 2, pageWidth - 1, 30);

    for (size_t i = 0; i < files.size(); i++) {
      const auto file = files[i];
      renderer.drawText(UI_FONT_ID, 20, 60 + i * 30, file.c_str(), i != selectorIndex);
    }
  }

  renderer.displayBuffer();
}
