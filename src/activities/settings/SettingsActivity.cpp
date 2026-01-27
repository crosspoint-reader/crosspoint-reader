#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <cstring>

#include "BluetoothManager.h"
#include "CalibreSettingsActivity.h"
#include "CategorySettingsActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Display", "Reader", "Controls", "System"};

namespace {
constexpr int settingsCount = 24;
const SettingInfo settingsList[settingsCount] = {
constexpr int displaySettingsCount = 5;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover", "None"}),
    SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"}),
    SettingInfo::Enum("Status Bar", &CrossPointSettings::statusBar, {"None", "No Progress", "Full"}),
    SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"}),
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"})};

constexpr int readerSettingsCount = 9;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic"}),
    SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}),
    SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}),
    SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                      {"Justify", "Left", "Center", "Right"}),
    SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
    SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Enum(
        "Front Button Layout", &CrossPointSettings::frontButtonLayout,
        {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght", "Bck, Cnfrm, Rght, Lft"}),
    SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                      {"Prev, Next", "Next, Prev"}),
    SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn, {"Ignore", "Sleep", "Page Turn"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                      {"1 min", "5 min", "10 min", "15 min", "30 min"}),
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Enum("Bluetooth", &CrossPointSettings::bluetoothEnabled, {"Off", "On"}),
    SettingInfo::Enum("Bluetooth Keyboard", &CrossPointSettings::bluetoothKeyboardEnabled, {"Disabled", "Enabled"}),
    SettingInfo::Action("KOReader Sync"),
    SettingInfo::Action("Calibre Settings"),
    SettingInfo::Action("Check for updates")};
}  // namespace

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
  if (categoryIndex < 0 || categoryIndex >= categoryCount) {
    return;
  }

  const auto& setting = settingsList[selectedSettingIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const uint8_t newValue = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    SETTINGS.*(setting.valuePtr) = newValue;
    
    // Handle Bluetooth toggle specifically
    if (strcmp(setting.name, "Bluetooth") == 0) {
      if (newValue == CrossPointSettings::BLUETOOTH_MODE::ON) {
        // Enable Bluetooth
        if (!BLUETOOTH_MANAGER.isInitialized()) {
          if (BLUETOOTH_MANAGER.initialize()) {
            BLUETOOTH_MANAGER.startAdvertising();
          } else {
            // Failed to initialize, revert to OFF
            SETTINGS.*(setting.valuePtr) = CrossPointSettings::BLUETOOTH_MODE::OFF;
          }
        }
      } else {
        // Disable Bluetooth
        if (BLUETOOTH_MANAGER.isInitialized()) {
          BLUETOOTH_MANAGER.shutdown();
        }
      }
    } else if (strcmp(setting.name, "Bluetooth Keyboard") == 0) {
      if (newValue == CrossPointSettings::BLUETOOTH_KEYBOARD_MODE::KBD_ENABLED) {
        // Enable keyboard requires Bluetooth to be on
        if (!BLUETOOTH_MANAGER.isInitialized()) {
          // Force Bluetooth on first
          SETTINGS.bluetoothEnabled = CrossPointSettings::BLUETOOTH_MODE::ON;
          if (!BLUETOOTH_MANAGER.initialize()) {
            // Failed, revert both to OFF
            SETTINGS.bluetoothEnabled = CrossPointSettings::BLUETOOTH_MODE::OFF;
            SETTINGS.*(setting.valuePtr) = CrossPointSettings::BLUETOOTH_KEYBOARD_MODE::KBD_DISABLED;
          }
        }
        
        // Initialize keyboard handler if not already done
        auto* keyboardHandler = BLUETOOTH_MANAGER.getKeyboardHandler();
        if (!keyboardHandler && BLUETOOTH_MANAGER.isInitialized()) {
          // This will be handled by BluetoothManager on next init
          BLUETOOTH_MANAGER.shutdown();
          BLUETOOTH_MANAGER.initialize();
        }
      } else {
        // Disable keyboard (but keep Bluetooth on)
        auto* keyboardHandler = BLUETOOTH_MANAGER.getKeyboardHandler();
        if (keyboardHandler) {
          keyboardHandler->shutdown();
          // Would need to reinit without keyboard to clean up properly
          BLUETOOTH_MANAGER.shutdown();
          BLUETOOTH_MANAGER.initialize();
        }
        
        // Force garbage collection to free keyboard memory
        BLUETOOTH_MANAGER.collectGarbage();
      }
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // Decreasing would also be nice for large ranges I think but oh well can't have everything
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    // Wrap to minValue if exceeding setting value boundary
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
    // Only toggle if it's a toggle type and has a value pointer
    return;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();

  const SettingInfo* settingsList = nullptr;
  int settingsCount = 0;

  switch (categoryIndex) {
    case 0:  // Display
      settingsList = displaySettings;
      settingsCount = displaySettingsCount;
      break;
    case 1:  // Reader
      settingsList = readerSettings;
      settingsCount = readerSettingsCount;
      break;
    case 2:  // Controls
      settingsList = controlsSettings;
      settingsCount = controlsSettingsCount;
      break;
    case 3:  // System
      settingsList = systemSettings;
      settingsCount = systemSettingsCount;
      break;
  }

  enterNewActivity(new CategorySettingsActivity(renderer, mappedInput, categoryNames[categoryIndex], settingsList,
                                                settingsCount, [this] {
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
