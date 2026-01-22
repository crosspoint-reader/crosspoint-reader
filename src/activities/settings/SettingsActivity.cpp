#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <algorithm>

#include "CategorySettingsActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Display", "Reader", "Controls", "System"};

// Helper function to find descriptor by member pointer
static const SettingDescriptor* findDescriptor(uint8_t CrossPointSettings::* memberPtr) {
  auto it = std::find_if(CrossPointSettings::descriptors.begin(), CrossPointSettings::descriptors.end(),
                         [memberPtr](const SettingDescriptor& desc) { return desc.memberPtr == memberPtr; });
  return (it != CrossPointSettings::descriptors.end()) ? &(*it) : nullptr;
}

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Reset selection to first category
  selectedCategoryIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,               // Stack size
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

  // Handle category selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    enterCategory(selectedCategoryIndex);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move selection up (with wrap-around)
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move selection down (with wrap around)
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  }
}

void SettingsActivity::enterCategory(int categoryIndex) {
  std::vector<const SettingDescriptor*> descriptors;
  std::vector<ActionItem> actionItems;

  switch (categoryIndex) {
    case 0:  // Display
      descriptors.push_back(findDescriptor(&CrossPointSettings::sleepScreen));
      descriptors.push_back(findDescriptor(&CrossPointSettings::sleepScreenCoverMode));
      descriptors.push_back(findDescriptor(&CrossPointSettings::sleepScreenCoverFilter));
      descriptors.push_back(findDescriptor(&CrossPointSettings::statusBar));
      descriptors.push_back(findDescriptor(&CrossPointSettings::hideBatteryPercentage));
      descriptors.push_back(findDescriptor(&CrossPointSettings::refreshFrequency));
      break;

    case 1:  // Reader
      descriptors.push_back(findDescriptor(&CrossPointSettings::fontFamily));
      descriptors.push_back(findDescriptor(&CrossPointSettings::fontSize));
      descriptors.push_back(findDescriptor(&CrossPointSettings::lineSpacing));
      descriptors.push_back(findDescriptor(&CrossPointSettings::screenMargin));
      descriptors.push_back(findDescriptor(&CrossPointSettings::paragraphAlignment));
      descriptors.push_back(findDescriptor(&CrossPointSettings::hyphenationEnabled));
      descriptors.push_back(findDescriptor(&CrossPointSettings::orientation));
      descriptors.push_back(findDescriptor(&CrossPointSettings::extraParagraphSpacing));
      descriptors.push_back(findDescriptor(&CrossPointSettings::textAntiAliasing));
      break;

    case 2:  // Controls
      descriptors.push_back(findDescriptor(&CrossPointSettings::frontButtonLayout));
      descriptors.push_back(findDescriptor(&CrossPointSettings::sideButtonLayout));
      descriptors.push_back(findDescriptor(&CrossPointSettings::longPressChapterSkip));
      descriptors.push_back(findDescriptor(&CrossPointSettings::shortPwrBtn));
      break;

    case 3:  // System
      descriptors.push_back(findDescriptor(&CrossPointSettings::sleepTimeout));
      actionItems.push_back({"KOReader Sync", ActionItem::Type::KOREADER_SYNC});
      actionItems.push_back({"Calibre Settings", ActionItem::Type::CALIBRE_SETTINGS});
      actionItems.push_back({"Clear Cache", ActionItem::Type::CLEAR_CACHE});
      actionItems.push_back({"Check for updates", ActionItem::Type::CHECK_UPDATES});
      break;
  }

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new CategorySettingsActivity(renderer, mappedInput, categoryNames[categoryIndex], descriptors,
                                                actionItems, [this] {
                                                  exitActivity();
                                                  updateRequired = true;
                                                }));
  xSemaphoreGive(renderingMutex);
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Settings", true, EpdFontFamily::BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectedCategoryIndex * 30 - 2, pageWidth - 1, 30);

  // Draw all categories
  for (int i = 0; i < categoryCount; i++) {
    const int categoryY = 60 + i * 30;  // 30 pixels between categories

    // Draw category name
    renderer.drawText(UI_10_FONT_ID, 20, categoryY, categoryNames[i], i != selectedCategoryIndex);
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
