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
#include "RefreshFrequencySelectionActivity.h"
#include "ScreenMarginSelectionActivity.h"
#include "SleepBmpSelectionActivity.h"
#include "SleepScreenSelectionActivity.h"
#include "SleepTimeoutSelectionActivity.h"
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

  // Handle navigation (skip hidden settings)
  const int visibleCount = getVisibleSettingsCount();
  if (visibleCount == 0) {
    return;  // No visible settings
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move to previous visible setting
    int currentActual = mapVisibleIndexToActualIndex(selectedSettingIndex);
    do {
      currentActual = (currentActual > 0) ? (currentActual - 1) : (settingsCount - 1);
    } while (!shouldShowSetting(currentActual) && currentActual != mapVisibleIndexToActualIndex(selectedSettingIndex));
    selectedSettingIndex = mapActualIndexToVisibleIndex(currentActual);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move to next visible setting
    int currentActual = mapVisibleIndexToActualIndex(selectedSettingIndex);
    do {
      currentActual = (currentActual < settingsCount - 1) ? (currentActual + 1) : 0;
    } while (!shouldShowSetting(currentActual) && currentActual != mapVisibleIndexToActualIndex(selectedSettingIndex));
    selectedSettingIndex = mapActualIndexToVisibleIndex(currentActual);
    updateRequired = true;
  }
}

void CategorySettingsActivity::toggleCurrentSetting() {
  const int actualIndex = mapVisibleIndexToActualIndex(selectedSettingIndex);
  if (actualIndex < 0 || actualIndex >= settingsCount) {
    return;
  }

  const auto& setting = settingsList[actualIndex];

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
    } else if (strcmp(setting.name, "Calibre Settings") == 0) {
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
    } else if (strcmp(setting.name, "Sleep Screen") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SleepScreenSelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Refresh Frequency") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new RefreshFrequencySelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Screen Margin") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ScreenMarginSelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Time to Sleep") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SleepTimeoutSelectionActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "Select Sleep BMP") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new SleepBmpSelectionActivity(renderer, mappedInput, [this] {
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

bool CategorySettingsActivity::shouldShowSetting(int index) const {
  if (index < 0 || index >= settingsCount) {
    return false;
  }
  // Hide "Select Sleep BMP" if sleep screen is not set to CUSTOM
  if (settingsList[index].type == SettingType::ACTION && 
      strcmp(settingsList[index].name, "Select Sleep BMP") == 0) {
    return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  }
  return true;
}

int CategorySettingsActivity::getVisibleSettingsCount() const {
  int count = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (shouldShowSetting(i)) {
      count++;
    }
  }
  return count;
}

int CategorySettingsActivity::mapVisibleIndexToActualIndex(int visibleIndex) const {
  int visibleCount = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (shouldShowSetting(i)) {
      if (visibleCount == visibleIndex) {
        return i;
      }
      visibleCount++;
    }
  }
  // If visibleIndex is out of bounds, return first visible setting
  for (int i = 0; i < settingsCount; i++) {
    if (shouldShowSetting(i)) {
      return i;
    }
  }
  return 0;  // Fallback
}

int CategorySettingsActivity::mapActualIndexToVisibleIndex(int actualIndex) const {
  int visibleIndex = 0;
  for (int i = 0; i < actualIndex; i++) {
    if (shouldShowSetting(i)) {
      visibleIndex++;
    }
  }
  return visibleIndex;
}

void CategorySettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, categoryName, true, EpdFontFamily::BOLD);

  // Calculate visible settings count and map selection
  const int visibleCount = getVisibleSettingsCount();
  const int actualSelectedIndex = mapVisibleIndexToActualIndex(selectedSettingIndex);
  
  // Draw selection highlight
  int visibleIndex = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (shouldShowSetting(i)) {
      if (i == actualSelectedIndex) {
        renderer.fillRect(0, 60 + visibleIndex * 30 - 2, pageWidth - 1, 30);
        break;
      }
      visibleIndex++;
    }
  }

  // Draw all visible settings
  visibleIndex = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (!shouldShowSetting(i)) {
      continue;
    }
    
    const int settingY = 60 + visibleIndex * 30;  // 30 pixels between settings
    const bool isSelected = (i == actualSelectedIndex);

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
    } else if (settingsList[i].type == SettingType::ACTION && strcmp(settingsList[i].name, "Sleep Screen") == 0) {
      valueText = CrossPointSettings::getSleepScreenString(SETTINGS.sleepScreen);
    } else if (settingsList[i].type == SettingType::ACTION && strcmp(settingsList[i].name, "Refresh Frequency") == 0) {
      valueText = CrossPointSettings::getRefreshFrequencyString(SETTINGS.refreshFrequency);
    } else if (settingsList[i].type == SettingType::ACTION && strcmp(settingsList[i].name, "Screen Margin") == 0) {
      // Format margin value as "X px"
      valueText = std::to_string(SETTINGS.screenMargin) + " px";
    } else if (settingsList[i].type == SettingType::ACTION && strcmp(settingsList[i].name, "Time to Sleep") == 0) {
      valueText = CrossPointSettings::getSleepTimeoutString(SETTINGS.sleepTimeout);
    } else if (settingsList[i].type == SettingType::ACTION && strcmp(settingsList[i].name, "Select Sleep BMP") == 0) {
      if (SETTINGS.selectedSleepBmp[0] != '\0') {
        valueText = SETTINGS.selectedSleepBmp;
        if (valueText.length() > 20) {
          valueText = valueText.substr(0, 17) + "...";
        }
      } else {
        valueText = "Random";
      }
    }
    if (!valueText.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(), !isSelected);
    }
    
    visibleIndex++;
  }

  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
