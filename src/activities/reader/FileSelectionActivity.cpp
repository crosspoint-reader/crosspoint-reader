#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int PAGE_ITEMS = 20;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int headerY = 16;
constexpr int separatorY = 48;
constexpr int listStartY = 60;
constexpr int rowHeight = 28;
constexpr int horizontalMargin = 16;
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

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
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
      std::string ext4 = filename.length() >= 4 ? filename.substr(filename.length() - 4) : "";
      std::string ext5 = filename.length() >= 5 ? filename.substr(filename.length() - 5) : "";
      if (ext5 == ".epub" || ext5 == ".xtch" || ext4 == ".xtc") {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileSelectionActivity::onEnter() {
  Activity::onEnter();
  lastRenderedPath.clear();  // Force HALF_REFRESH on first render

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
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      loadFiles();
      updateRequired = true;
      xSemaphoreGive(renderingMutex);
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      loadFiles();
      updateRequired = true;
      xSemaphoreGive(renderingMutex);
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        loadFiles();
        updateRequired = true;
        xSemaphoreGive(renderingMutex);
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
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    updateRequired = true;
    xSemaphoreGive(renderingMutex);
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    updateRequired = true;
    xSemaphoreGive(renderingMutex);
  }
}

void FileSelectionActivity::displayTaskLoop() {
  while (true) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (updateRequired) {
      updateRequired = false;
      render();
    }
    xSemaphoreGive(renderingMutex);
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FileSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header with path
  const std::string pathDisplay = basepath == "/" ? "Browse" : basepath;
  const auto truncatedPath = renderer.truncatedText(UI_12_FONT_ID, pathDisplay.c_str(),
                                                     pageWidth - horizontalMargin * 2, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, headerY, truncatedPath.c_str(), true, EpdFontFamily::BOLD);

  // Subtle separator line under header
  renderer.drawLine(horizontalMargin, separatorY, pageWidth - horizontalMargin, separatorY);

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    const int emptyY = listStartY + 40;
    renderer.drawCenteredText(UI_10_FONT_ID, emptyY, "No files found");
    renderer.drawCenteredText(SMALL_FONT_ID, emptyY + 24, "Supported: .epub, .xtc, .xtch");
    // Use HALF_REFRESH when directory changed
    if (basepath != lastRenderedPath) {
      lastRenderedPath = basepath;
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer();
    }
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, listStartY + (selectorIndex % PAGE_ITEMS) * rowHeight - 2, pageWidth - 1, rowHeight);
  for (int i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), pageWidth - horizontalMargin * 2 - 8);
    renderer.drawText(UI_10_FONT_ID, horizontalMargin + 4, listStartY + (i % PAGE_ITEMS) * rowHeight, item.c_str(),
                      i != selectorIndex);
  }

  // Use HALF_REFRESH when directory changed (basepath differs from last render)
  if (basepath != lastRenderedPath) {
    lastRenderedPath = basepath;
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}
