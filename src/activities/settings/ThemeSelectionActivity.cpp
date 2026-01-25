#include "ThemeSelectionActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <cstring>
#include <esp_system.h>

void ThemeSelectionActivity::taskTrampoline(void *param) {
  auto *self = static_cast<ThemeSelectionActivity *>(param);
  self->displayTaskLoop();
}

void ThemeSelectionActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Load themes
  themeNames.clear();
  // Always add Default
  themeNames.push_back("Default");

  FsFile root = SdMan.open("/themes");
  if (root.isDirectory()) {
    FsFile file;
    while (file.openNext(&root, O_RDONLY)) {
      if (file.isDirectory()) {
        char name[256];
        file.getName(name, sizeof(name));
        // Skip hidden folders and "Default" if scans it (already added)
        if (name[0] != '.' && std::string(name) != "Default") {
          themeNames.push_back(name);
        }
      }
      file.close();
    }
  }
  root.close();

  // Find current selection
  std::string current = SETTINGS.themeName;
  selectedIndex = 0;
  for (size_t i = 0; i < themeNames.size(); i++) {
    if (themeNames[i] == current) {
      selectedIndex = i;
      break;
    }
  }

  updateRequired = true;
  xTaskCreate(&ThemeSelectionActivity::taskTrampoline, "ThemeSelTask", 4096,
              this, 1, &displayTaskHandle);
}

void ThemeSelectionActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ThemeSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < themeNames.size()) {
      std::string selected = themeNames[selectedIndex];

      // Only reboot if theme actually changed
      if (selected != std::string(SETTINGS.themeName)) {
        strncpy(SETTINGS.themeName, selected.c_str(),
                sizeof(SETTINGS.themeName) - 1);
        SETTINGS.themeName[sizeof(SETTINGS.themeName) - 1] = '\0';
        SETTINGS.saveToFile();

        // Show reboot message
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20,
                                  "Applying theme...", true);
        renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 10,
                                  "Device will restart", true);
        renderer.displayBuffer();

        // Small delay to ensure display updates
        vTaskDelay(500 / portTICK_PERIOD_MS);

        esp_restart();
        return;
      }
    }
    onGoBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex =
        (selectedIndex > 0) ? (selectedIndex - 1) : (themeNames.size() - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex =
        (selectedIndex < themeNames.size() - 1) ? (selectedIndex + 1) : 0;
    updateRequired = true;
  }
}

void ThemeSelectionActivity::displayTaskLoop() {
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

void ThemeSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Theme", true,
                            EpdFontFamily::BOLD);

  // Layout constants
  const int entryHeight = 30;
  const int startY = 60;
  const int maxVisible = (pageHeight - startY - 40) / entryHeight;

  // Viewport calculation
  int startIdx = 0;
  if (themeNames.size() > maxVisible) {
    if (selectedIndex >= maxVisible / 2) {
      startIdx = selectedIndex - maxVisible / 2;
    }
    if (startIdx + maxVisible > themeNames.size()) {
      startIdx = themeNames.size() - maxVisible;
    }
  }

  // Draw Highlight
  int visibleIndex = selectedIndex - startIdx;
  if (visibleIndex >= 0 && visibleIndex < maxVisible) {
    renderer.fillRect(0, startY + visibleIndex * entryHeight - 2, pageWidth - 1,
                      entryHeight);
  }

  // Draw List
  for (int i = 0; i < maxVisible && (startIdx + i) < themeNames.size(); i++) {
    int idx = startIdx + i;
    int y = startY + i * entryHeight;
    bool isSelected = (idx == selectedIndex);

    std::string displayName = themeNames[idx];
    if (themeNames[idx] == std::string(SETTINGS.themeName)) {
      displayName = "* " + displayName;
    }
    renderer.drawText(UI_10_FONT_ID, 20, y, displayName.c_str(), !isSelected);
  }

  // Scrollbar if needed
  if (themeNames.size() > maxVisible) {
    int barHeight = pageHeight - startY - 40;
    int thumbHeight = barHeight * maxVisible / themeNames.size();
    int thumbY = startY + (barHeight - thumbHeight) * startIdx /
                              (themeNames.size() - maxVisible);
    renderer.fillRect(pageWidth - 5, startY, 2, barHeight, 0);
    renderer.fillRect(pageWidth - 7, thumbY, 6, thumbHeight, 1);
  }

  const auto labels = mappedInput.mapLabels("Cancel", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3,
                           labels.btn4);

  renderer.displayBuffer();
}
