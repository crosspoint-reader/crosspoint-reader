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
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long IGNORE_INPUT_MS = 300;  // Ignore input for 300ms after entering
constexpr int LINE_HEIGHT = 30;
constexpr int START_Y = 60;
constexpr int BOTTOM_BAR_HEIGHT = 60;  // Space for button hints

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    return std::lexicographical_compare(begin(str1), end(str1), begin(str2), end(str2),
                                       [](const char& char1, const char& char2) {
                                         return std::tolower(char1) < std::tolower(char2);
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
  
  std::vector<std::string> bmpFiles;
  
  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    char name[500];
    
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      
      if (filename[0] == '.' || filename.length() < 4 || 
          filename.substr(filename.length() - 4) != ".bmp") {
        file.close();
        continue;
      }
      
      // Validate BMP
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        file.close();
        continue;
      }
      file.close();
      
      bmpFiles.emplace_back(filename);
    }
    dir.close();
    
    // Sort alphabetically (case-insensitive)
    sortFileList(bmpFiles);
  }
  
  // Add "Random" as first option, then sorted BMP files
  files.emplace_back("Random");
  files.insert(files.end(), bmpFiles.begin(), bmpFiles.end());
}

void SleepBmpSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();
  
  // Set initial selection: "Random" if no file selected, otherwise find the selected file
  if (SETTINGS.selectedSleepBmp[0] == '\0') {
    selectorIndex = 0;  // "Random" is at index 0
  } else {
    // Find the selected file in the sorted list
    selectorIndex = 0;  // Default to "Random" if not found
    for (size_t i = 1; i < files.size(); i++) {
      if (files[i] == SETTINGS.selectedSleepBmp) {
        selectorIndex = i;
        break;
      }
    }
  }
  
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
    if (files.empty() || selectorIndex >= files.size()) {
      return;
    }

    const std::string selectedFile = files[selectorIndex];
    if (selectedFile == "Random") {
      // Clear the selection to use random
      SETTINGS.selectedSleepBmp[0] = '\0';
    } else {
      strncpy(SETTINGS.selectedSleepBmp, selectedFile.c_str(), sizeof(SETTINGS.selectedSleepBmp) - 1);
      SETTINGS.selectedSleepBmp[sizeof(SETTINGS.selectedSleepBmp) - 1] = '\0';
    }
    SETTINGS.saveToFile();

    onBack();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
  } else if (prevReleased) {
    if (files.empty()) {
      return;
    }
    
    // Calculate items per page dynamically
    const int screenHeight = renderer.getScreenHeight();
    const int availableHeight = screenHeight - START_Y - BOTTOM_BAR_HEIGHT;
    const int pageItems = (availableHeight / LINE_HEIGHT);
    
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (files.empty()) {
      return;
    }
    
    // Calculate items per page dynamically
    const int screenHeight = renderer.getScreenHeight();
    const int availableHeight = screenHeight - START_Y - BOTTOM_BAR_HEIGHT;
    const int pageItems = (availableHeight / LINE_HEIGHT);
    
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % files.size();
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

  // Calculate items per page based on screen height
  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - START_Y - BOTTOM_BAR_HEIGHT;
  const int pageItems = (availableHeight / LINE_HEIGHT);
  
  // Calculate page start index
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  
  // Draw selection highlight
  const int visibleSelectedIndex = static_cast<int>(selectorIndex - pageStartIndex);
  if (visibleSelectedIndex >= 0 && visibleSelectedIndex < pageItems && selectorIndex < files.size()) {
    renderer.fillRect(0, START_Y + visibleSelectedIndex * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);
  }
  
  // Draw visible files
  int visibleIndex = 0;
  for (size_t i = pageStartIndex; i < files.size() && visibleIndex < pageItems; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    const bool isSelected = (i == selectorIndex);
    renderer.drawText(UI_10_FONT_ID, 20, START_Y + visibleIndex * LINE_HEIGHT, item.c_str(), !isSelected);
    visibleIndex++;
  }

  renderer.displayBuffer();
}

