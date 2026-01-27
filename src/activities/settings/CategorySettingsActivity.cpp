#include "CategorySettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <cstring>

#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "fontIds.h"

void CategorySettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CategorySettingsActivity*>(param);
  self->displayTaskLoop();
}

void CategorySettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  selectedSettingIndex = 0;
  updateRequired = true;

  xTaskCreate(&CategorySettingsActivity::taskTrampoline, "CategorySettingsActivityTask", 4096, this, 1,
              &displayTaskHandle);
}

void CategorySettingsActivity::onExit() {
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

void CategorySettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoBack();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedSettingIndex = (selectedSettingIndex < settingsCount - 1) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }
}

void CategorySettingsActivity::toggleCurrentSetting() {
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
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "KOReader Sync") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "OPDS Browser") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Clear Cache") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Check for updates") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    }
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void CategorySettingsActivity::displayTaskLoop() {
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

void CategorySettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, categoryName, true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedSettingIndex * 30 - 2, pageWidth - 1, 30);

  // Draw all settings
  for (int i = 0; i < settingsCount; i++) {
    const int settingY = 60 + i * 30;  // 30 pixels between settings
    const bool isSelected = (i == selectedSettingIndex);

    // Draw setting name
    renderer.drawText(UI_10_FONT_ID, 20, settingY, settingsList[i].name, !isSelected);

    // Draw value based on setting type
    std::string valueText;
    if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
      const bool value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = value ? "ON" : "OFF";
    } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = settingsList[i].enumValues[value];
    } else if (settingsList[i].type == SettingType::VALUE && settingsList[i].valuePtr != nullptr) {
      valueText = std::to_string(SETTINGS.*(settingsList[i].valuePtr));
    }
    if (!valueText.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(), !isSelected);
    }
  }

  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
