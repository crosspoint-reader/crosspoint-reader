#include "SleepBmpSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/StringUtils.h"

#include "../../../lib/GfxRenderer/Bitmap.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long IGNORE_INPUT_MS = 300;  // Ignore input for 300ms after entering

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    return lexicographical_compare(begin(str1), end(str1), begin(str2), end(str2),
                                   [](const char& char1, const char& char2) {
                                     return tolower(char1) < tolower(char2);
                                   });
  });
}
}  // namespace

void SleepBmpSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SleepBmpSelectionActivity*>(param);
  self->displayTaskLoop();
}

void SleepBmpSelectionActivity::loadFiles() {
  files.clear();

  auto dir = SdMan.open("/sleep");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  dir.rewindDirectory();

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    auto filename = std::string(name);
    if (filename[0] == '.') {
      file.close();
      continue;
    }

    if (filename.substr(filename.length() - 4) != ".bmp") {
      file.close();
      continue;
    }

    Bitmap bitmap(file);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      continue;
    }
    file.close();

    files.emplace_back(filename);
  }
  dir.close();
  sortFileList(files);
}

void SleepBmpSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();
  selectorIndex = 0;
  enterTime = millis();

  updateRequired = true;

  xTaskCreate(&SleepBmpSelectionActivity::taskTrampoline, "SleepBmpSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SleepBmpSelectionActivity::onExit() {
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

void SleepBmpSelectionActivity::loop() {
  const unsigned long timeSinceEnter = millis() - enterTime;
  if (timeSinceEnter < IGNORE_INPUT_MS) {
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

    const std::string selectedFile = files[selectorIndex];
    strncpy(SETTINGS.selectedSleepBmp, selectedFile.c_str(), sizeof(SETTINGS.selectedSleepBmp) - 1);
    SETTINGS.selectedSleepBmp[sizeof(SETTINGS.selectedSleepBmp) - 1] = '\0';
    SETTINGS.saveToFile();

    onBack();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
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

void SleepBmpSelectionActivity::displayTaskLoop() {
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

void SleepBmpSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Sleep BMP", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No BMP files found in /sleep");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
  for (size_t i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }

  renderer.displayBuffer();
}

