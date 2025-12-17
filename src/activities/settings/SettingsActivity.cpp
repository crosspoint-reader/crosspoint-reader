#include "SettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "config.h"

// Define the static settings list

const SettingInfo SettingsActivity::settingsList[settingsCount] = {
    {"White Sleep Screen", &CrossPointSettings::whiteSleepScreen},
    {"Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing}};

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection to first item
  selectedSettingIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void SettingsActivity::onExit() {
  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void SettingsActivity::loop() {
  // Handle actions with early return
  if (inputManager.wasPressed(InputManager::BTN_CONFIRM)) {
    toggleCurrentSetting();
    updateRequired = true;
    return;
  }

  if (inputManager.wasPressed(InputManager::BTN_BACK)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  // Handle navigation
  if (inputManager.wasPressed(InputManager::BTN_UP) || inputManager.wasPressed(InputManager::BTN_LEFT)) {
    // Move selection up (with wrap-around)
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount - 1);
    updateRequired = true;
  } else if (inputManager.wasPressed(InputManager::BTN_DOWN) || inputManager.wasPressed(InputManager::BTN_RIGHT)) {
    // Move selection down (with wrap-around)
    selectedSettingIndex = (selectedSettingIndex < settingsCount - 1) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }
}

void SettingsActivity::toggleCurrentSetting() {
  // Validate index
  if (selectedSettingIndex < 0 || selectedSettingIndex >= settingsCount) {
    return;
  }

  // Toggle the boolean value using the member pointer
  bool currentValue = SETTINGS.*(settingsList[selectedSettingIndex].valuePtr);
  SETTINGS.*(settingsList[selectedSettingIndex].valuePtr) = !currentValue;

  // Save settings when they change
  SETTINGS.saveToFile();
}

void SettingsActivity::displayTaskLoop() {
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

void SettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  // Draw header
  renderer.drawCenteredText(READER_FONT_ID, 10, "Settings", true, BOLD);

  // We always have at least one setting

  // Draw all settings
  for (int i = 0; i < settingsCount; i++) {
    const int settingY = 60 + i * 30;  // 30 pixels between settings

    // Draw selection indicator for the selected setting
    if (i == selectedSettingIndex) {
      renderer.drawText(UI_FONT_ID, 5, settingY, ">");
    }

    // Draw setting name and value
    renderer.drawText(UI_FONT_ID, 20, settingY, settingsList[i].name);
    bool value = SETTINGS.*(settingsList[i].valuePtr);
    renderer.drawText(UI_FONT_ID, pageWidth - 80, settingY, value ? "ON" : "OFF");
  }

  // Draw help text
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 30, "Press OK to toggle, BACK to save & exit");

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
