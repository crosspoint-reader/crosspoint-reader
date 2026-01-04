#include "SettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

// Define the static settings list
namespace {
constexpr int settingsCount = 18;
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
    {"Reader Paragraph Alignment",
     SettingType::ENUM,
     &CrossPointSettings::paragraphAlignment,
     {"Justify", "Left", "Center", "Right"}},
    {"Time to Sleep",
     SettingType::ENUM,
     &CrossPointSettings::sleepTimeout,
     {"1 min", "5 min", "10 min", "15 min", "30 min"}},
    {"Refresh Frequency",
     SettingType::ENUM,
     &CrossPointSettings::refreshFrequency,
     {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}},
    {"KOReader Username", SettingType::ACTION, nullptr, {}},
    {"KOReader Password", SettingType::ACTION, nullptr, {}},
    {"Authenticate KOReader", SettingType::ACTION, nullptr, {}},
    {"Check for updates", SettingType::ACTION, nullptr, {}},
};

// Check if a setting should be visible
bool isSettingVisible(int index) {
  // Hide "Authenticate KOReader" if credentials are not set
  if (std::string(settingsList[index].name) == "Authenticate KOReader") {
    return KOREADER_STORE.hasCredentials();
  }
  return true;
}

// Get visible settings count
int getVisibleSettingsCount() {
  int count = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (isSettingVisible(i)) {
      count++;
    }
  }
  return count;
}

// Convert visible index to actual settings index
int visibleToActualIndex(int visibleIndex) {
  int count = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (isSettingVisible(i)) {
      if (count == visibleIndex) {
        return i;
      }
      count++;
    }
  }
  return -1;
}

// Convert actual index to visible index
int actualToVisibleIndex(int actualIndex) {
  int count = 0;
  for (int i = 0; i < actualIndex; i++) {
    if (isSettingVisible(i)) {
      count++;
    }
  }
  return count;
}
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

  // Handle navigation (using visible settings count)
  const int visibleCount = getVisibleSettingsCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move selection up (with wrap-around)
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (visibleCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move selection down
    if (selectedSettingIndex < visibleCount - 1) {
      selectedSettingIndex++;
      updateRequired = true;
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  // Convert visible index to actual index
  const int actualIndex = visibleToActualIndex(selectedSettingIndex);
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
  } else if (setting.type == SettingType::ACTION) {
    if (std::string(setting.name) == "Check for updates") {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (std::string(setting.name) == "KOReader Username") {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "KOReader Username", KOREADER_STORE.getUsername(), 10,
          64,     // maxLength
          false,  // not password
          [this](const std::string& username) {
            KOREADER_STORE.setCredentials(username, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
    } else if (std::string(setting.name) == "KOReader Password") {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "KOReader Password", KOREADER_STORE.getPassword(), 10,
          64,     // maxLength
          false,  // not password mode - show characters
          [this](const std::string& password) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), password);
            KOREADER_STORE.saveToFile();
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
    } else if (std::string(setting.name) == "Authenticate KOReader") {
      // Only allow if credentials are set
      if (!KOREADER_STORE.hasCredentials()) {
        return;
      }
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderAuthActivity(renderer, mappedInput, [this] {
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

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Settings", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedSettingIndex * 30 - 2, pageWidth - 1, 30);

  // Draw only visible settings
  int visibleIndex = 0;
  for (int i = 0; i < settingsCount; i++) {
    if (!isSettingVisible(i)) {
      continue;
    }

    const int settingY = 60 + visibleIndex * 30;  // 30 pixels between settings
    const bool isSelected = (visibleIndex == selectedSettingIndex);

    // Draw setting name
    renderer.drawText(UI_10_FONT_ID, 20, settingY, settingsList[i].name, !isSelected);

    // Draw value based on setting type
    std::string valueText = "";
    if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
      const bool value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = value ? "ON" : "OFF";
    } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
      const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
      valueText = settingsList[i].enumValues[value];
    } else if (settingsList[i].type == SettingType::ACTION) {
      // Show status for KOReader settings
      if (std::string(settingsList[i].name) == "KOReader Username") {
        valueText = KOREADER_STORE.getUsername().empty() ? "[Not Set]" : "[Set]";
      } else if (std::string(settingsList[i].name) == "KOReader Password") {
        valueText = KOREADER_STORE.getPassword().empty() ? "[Not Set]" : "[Set]";
      }
    }
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(), !isSelected);

    visibleIndex++;
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Save", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
