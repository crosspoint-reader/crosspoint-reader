#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* SettingsActivity::categoryNames[categoryCount] = {"Display", "Reader", "Controls", "System"};

namespace {
constexpr int changeTabsMs = 700;
constexpr int displaySettingsCount = 8;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen,
                      {"Dark", "Light", "Custom", "Cover", "None", "Cover + Custom"}),
    SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"}),
    SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                      {"None", "Contrast", "Inverted"}),
    SettingInfo::Enum(
        "Status Bar", &CrossPointSettings::statusBar,
        {"None", "No Progress", "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar"}),
    SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"}),
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Enum("UI Theme", &CrossPointSettings::uiTheme, {"Classic", "Lyra"}),
    SettingInfo::Toggle("Sunlight Fading Fix", &CrossPointSettings::fadingFix),
};

constexpr int readerSettingsCount = 10;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily, {"Bookerly", "Noto Sans", "Open Dyslexic"}),
    SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize, {"Small", "Medium", "Large", "X Large"}),
    SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing, {"Tight", "Normal", "Wide"}),
    SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum("Paragraph Alignment", &CrossPointSettings::paragraphAlignment,
                      {"Justify", "Left", "Center", "Right", "Book's Style"}),
    SettingInfo::Toggle("Embedded Style", &CrossPointSettings::embeddedStyle),
    SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum("Reading Orientation", &CrossPointSettings::orientation,
                      {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
    SettingInfo::Toggle("Extra Paragraph Spacing", &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Text Anti-Aliasing", &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    // Launches the remap wizard for front buttons.
    SettingInfo::Action("Remap Front Buttons"),
    SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                      {"Prev, Next", "Next, Prev"}),
    SettingInfo::Toggle("Long-press Chapter Skip", &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Short Power Button Click", &CrossPointSettings::shortPwrBtn, {"Ignore", "Sleep", "Page Turn"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                      {"1 min", "5 min", "10 min", "15 min", "30 min"}),
    SettingInfo::Action("KOReader Sync"), SettingInfo::Action("OPDS Browser"), SettingInfo::Action("Clear Cache"),
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
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  settingsList = displaySettings;
  settingsCount = displaySettingsCount;

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

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      updateRequired = true;
    } else {
      toggleCurrentSetting();
      updateRequired = true;
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool changeTab = mappedInput.getHeldTime() > changeTabsMs;

  // Handle navigation
  if (upReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    updateRequired = true;
  } else if (downReleased && changeTab) {
    hasChangedCategory = true;
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    updateRequired = true;
  } else if (upReleased || leftReleased) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (settingsCount);
    updateRequired = true;
  } else if (rightReleased || downReleased) {
    selectedSettingIndex = (selectedSettingIndex < settingsCount) ? (selectedSettingIndex + 1) : 0;
    updateRequired = true;
  }

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
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
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = settingsList[selectedSetting];

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
    if (strcmp(setting.name, "Remap Front Buttons") == 0) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (strcmp(setting.name, "KOReader Sync") == 0) {
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

  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Settings");

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({categoryNames[i], selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, [this](int index) { return std::string(settingsList[index].name); },
      nullptr, nullptr,
      [this](int i) {
        const auto& setting = settingsList[i];
        std::string valueText = "";
        if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
          const bool value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = value ? "ON" : "OFF";
        } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = settingsList[i].enumValues[value];
        } else if (settingsList[i].type == SettingType::VALUE && settingsList[i].valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(settingsList[i].valuePtr));
        }
        return valueText;
      });

  // Draw version text
  renderer.drawText(SMALL_FONT_ID,
                    pageWidth - metrics.versionTextRightX - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    metrics.versionTextY, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Toggle", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
