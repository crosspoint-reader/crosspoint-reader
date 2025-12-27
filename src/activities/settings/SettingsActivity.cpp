#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <InputManager.h>

#include "CrossPointSettings.h"
#include "OtaUpdateActivity.h"
#include "config.h"

// Define the static settings list
namespace {
constexpr int settingsCount = 5;
const SettingInfo settingsList[settingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    {"Sleep Screen", SettingType::ENUM, &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover"}},
    {"Status Bar", SettingType::ENUM, &CrossPointSettings::statusBar, {"None", "No Progress", "Full"}},
    {"Extra Paragraph Spacing", SettingType::TOGGLE, &CrossPointSettings::extraParagraphSpacing, {}},
    {"Short Power Button Click", SettingType::TOGGLE, &CrossPointSettings::shortPwrBtn, {}},
    {"Check for updates", SettingType::ACTION, nullptr, {}},
};
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

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
  ActivityWithSubactivity::onExit();

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
  if (subActivity) {
    subActivity->loop();
    return;
  }

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
    // Move selection down
    if (selectedSettingIndex < settingsCount - 1) {
      selectedSettingIndex++;
      updateRequired = true;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  // Validate index
  if (selectedSettingIndex < 0 || selectedSettingIndex >= settingsCount) {
    return;
  }

  const auto& setting = settingsList[selectedSettingIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ACTION) {
    if (std::string(setting.name) == "Check for updates") {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, inputManager, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    }
  } else {
    // Only toggle if it's a toggle type and has a value pointer
    return;
  }

  // Save settings when they change
  SETTINGS.saveToFile();
}

void SettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
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

  // Draw all settings
  for (int i = 0; i < settingsCount; i++) {
    const int settingY = 60 + i * 30;  // 30 pixels between settings

    // Draw selection indicator for the selected setting
    if (i == selectedSettingIndex) {
      renderer.drawText(UI_FONT_ID, 5, settingY, ">");
    }

    // Draw setting name
    renderer.drawText(UI_FONT_ID, 20, settingY, settingsList[i].name);

    // Draw value based on setting type
    if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
      const bool value = SETTINGS.*(settingsList[i].valuePtr);
      renderer.drawText(UI_FONT_ID, pageWidth - 80, settingY, value ? "ON" : "OFF");
    } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
      auto valueText = settingsList[i].enumValues[value];
      const auto width = renderer.getTextWidth(UI_FONT_ID, valueText.c_str());
      renderer.drawText(UI_FONT_ID, pageWidth - 50 - width, settingY, valueText.c_str());
    }
  }

  // Draw help text
  renderer.drawButtonHints(UI_FONT_ID, "« Save", "Toggle", "", "");
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 30, CROSSPOINT_VERSION);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
