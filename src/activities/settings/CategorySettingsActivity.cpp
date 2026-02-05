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
  const int totalItemsCount = descriptors.size() + actionItems.size();

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (totalItemsCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedSettingIndex = (selectedSettingIndex < totalItemsCount - 1) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }
}

void CategorySettingsActivity::toggleCurrentSetting() {
  const int totalItemsCount = descriptors.size() + actionItems.size();

  if (selectedSettingIndex < 0 || selectedSettingIndex >= totalItemsCount) {
    return;
  }

  // Check if it's a descriptor or an action item
  if (selectedSettingIndex < static_cast<int>(descriptors.size())) {
    // Handle descriptor
    const auto* desc = descriptors[selectedSettingIndex];

    if (desc->type == SettingType::TOGGLE) {
      uint8_t currentValue = desc->getValue(SETTINGS);
      desc->setValue(SETTINGS, !currentValue);
    } else if (desc->type == SettingType::ENUM) {
      uint8_t currentValue = desc->getValue(SETTINGS);
      desc->setValue(SETTINGS, (currentValue + 1) % desc->enumData.count);
    } else if (desc->type == SettingType::VALUE) {
      uint8_t currentValue = desc->getValue(SETTINGS);
      if (currentValue + desc->valueRange.step > desc->valueRange.max) {
        desc->setValue(SETTINGS, desc->valueRange.min);
      } else {
        desc->setValue(SETTINGS, currentValue + desc->valueRange.step);
      }
    }

    SETTINGS.saveToFile();
  } else {
    // Handle action item
    const int actionIndex = selectedSettingIndex - descriptors.size();
    const auto& action = actionItems[actionIndex];

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();

    switch (action.type) {
      case ActionItem::Type::KOREADER_SYNC:
        enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
          exitActivity();
          updateRequired = true;
        }));
        break;
      case ActionItem::Type::CALIBRE_SETTINGS:
        enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
          exitActivity();
          updateRequired = true;
        }));
        break;
      case ActionItem::Type::CLEAR_CACHE:
        enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
          exitActivity();
          updateRequired = true;
        }));
        break;
      case ActionItem::Type::CHECK_UPDATES:
        enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
          exitActivity();
          updateRequired = true;
        }));
        break;
    }

    xSemaphoreGive(renderingMutex);
  }
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

  // Draw header with category name
  renderer.drawCenteredText(UI_12_FONT_ID, 15, categoryName, true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedSettingIndex * 30 - 2, pageWidth - 1, 30);

  // Draw all descriptors
  for (size_t i = 0; i < descriptors.size(); i++) {
    const auto* desc = descriptors[i];
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedSettingIndex);

    // Draw setting name
    renderer.drawText(UI_10_FONT_ID, 20, settingY, desc->name, !isSelected);

    // Draw value based on setting type
    std::string valueText;
    if (desc->type == SettingType::TOGGLE) {
      const bool value = desc->getValue(SETTINGS);
      valueText = value ? "ON" : "OFF";
    } else if (desc->type == SettingType::ENUM) {
      const uint8_t value = desc->getValue(SETTINGS);
      valueText = desc->getEnumValueString(value);
    } else if (desc->type == SettingType::VALUE) {
      valueText = std::to_string(desc->getValue(SETTINGS));
    }
    if (!valueText.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(), !isSelected);
    }
  }

  // Draw all action items
  for (size_t i = 0; i < actionItems.size(); i++) {
    const auto& action = actionItems[i];
    const int itemIndex = descriptors.size() + i;
    const int settingY = 60 + itemIndex * 30;
    const bool isSelected = (itemIndex == selectedSettingIndex);

    // Draw action name
    renderer.drawText(UI_10_FONT_ID, 20, settingY, action.name, !isSelected);
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
