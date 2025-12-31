#include "SettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "fontIds.h"

// Define the static settings list
namespace {
constexpr int settingsCount = 13;
const SettingInfo settingsList[settingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    {"Sleep Screen", SettingType::ENUM, &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover"}},
    {"Status Bar", SettingType::ENUM, &CrossPointSettings::statusBar, {"None", "No Progress", "Full"}},
    {"Extra Paragraph Spacing", SettingType::TOGGLE, &CrossPointSettings::extraParagraphSpacing, {}},
    {"Short Power Button Click", SettingType::TOGGLE, &CrossPointSettings::shortPwrBtn, {}},
    {"Reading Orientation",
     SettingType::ENUM,
     &CrossPointSettings::orientation,
     {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}},
    {"Front Button Layout",
     SettingType::ENUM,
     &CrossPointSettings::frontButtonLayout,
     {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght"}},
    {"Side Button Layout (reader)",
     SettingType::ENUM,
     &CrossPointSettings::sideButtonLayout,
     {"Prev, Next", "Next, Prev"}},
    {"Reader Font Family",
     SettingType::ENUM,
     &CrossPointSettings::fontFamily,
     {"Bookerly", "Noto Sans", "Open Dyslexic"}},
    {"Reader Font Size", SettingType::ENUM, &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}},
    {"Reader Line Spacing", SettingType::ENUM, &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}},
    {"Time to Sleep",
     SettingType::ENUM,
     &CrossPointSettings::sleepTimeout,
     {"1 min", "5 min", "10 min", "15 min", "30 min"}},
    {"Refresh Frequency",
     SettingType::ENUM,
     &CrossPointSettings::refreshFrequency,
     {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}},
    {"Check for updates", SettingType::ACTION, nullptr, {}},
};
}  // namespace

void SettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SettingsActivity*>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  isFirstRender = true;

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
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
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
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
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
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Layout constants
  constexpr int headerY = 16;
  constexpr int separatorY = 42;
  constexpr int listStartY = 54;
  constexpr int rowHeight = 28;
  constexpr int horizontalMargin = 16;

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, headerY, "Settings", true, EpdFontFamily::BOLD);

  // Subtle separator line under header
  renderer.drawLine(horizontalMargin, separatorY, pageWidth - horizontalMargin, separatorY);

  // Draw selection highlight
  renderer.fillRect(0, listStartY + selectedSettingIndex * rowHeight - 2, pageWidth - 1, rowHeight);

  // Draw all settings
  for (int i = 0; i < settingsCount; i++) {
    const int settingY = listStartY + i * rowHeight;

    // Draw setting name
    renderer.drawText(UI_10_FONT_ID, horizontalMargin + 4, settingY, settingsList[i].name, i != selectedSettingIndex);

    // Draw value based on setting type
    std::string valueText = "";
    if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
      const bool value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = value ? "ON" : "OFF";
    } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = settingsList[i].enumValues[value];
    }
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - horizontalMargin - 4 - width, settingY, valueText.c_str(),
                      i != selectedSettingIndex);
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 16 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 58, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Save", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Use HALF_REFRESH on first render to clear ghosting, then FAST_REFRESH
  if (isFirstRender) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    isFirstRender = false;
  } else {
    renderer.displayBuffer();
  }
}
