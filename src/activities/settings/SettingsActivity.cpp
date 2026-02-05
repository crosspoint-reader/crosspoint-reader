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
#include "LanguageSelectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::CAT_DISPLAY, StrId::CAT_READER,
                                                              StrId::CAT_CONTROLS, StrId::CAT_SYSTEM};

namespace {
constexpr int changeTabsMs = 700;
constexpr int displaySettingsCount = 8;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum(StrId::SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                      {StrId::DARK, StrId::LIGHT, StrId::CUSTOM, StrId::COVER, StrId::NONE_OPT, StrId::COVER_CUSTOM}),
    SettingInfo::Enum(StrId::SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode, {StrId::FIT, StrId::CROP}),
    SettingInfo::Enum(StrId::SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                      {StrId::NONE_OPT, StrId::FILTER_CONTRAST, StrId::INVERTED}),
    SettingInfo::Enum(
        StrId::STATUS_BAR, &CrossPointSettings::statusBar,
        {StrId::NONE_OPT, StrId::NO_PROGRESS, StrId::STATUS_BAR_FULL_PERCENT, StrId::STATUS_BAR_FULL_BOOK, StrId::STATUS_BAR_BOOK_ONLY, StrId::STATUS_BAR_FULL_CHAPTER}),
    SettingInfo::Enum(StrId::HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage, {StrId::NEVER, StrId::IN_READER, StrId::ALWAYS}),
    SettingInfo::Enum(StrId::REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                      {StrId::PAGES_1, StrId::PAGES_5, StrId::PAGES_10, StrId::PAGES_15, StrId::PAGES_30}),
    SettingInfo::Enum(StrId::UI_THEME, &CrossPointSettings::uiTheme, {StrId::THEME_CLASSIC, StrId::THEME_LYRA}),
    SettingInfo::Toggle(StrId::SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix),
};

constexpr int readerSettingsCount = 9;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum(StrId::FONT_FAMILY, &CrossPointSettings::fontFamily, {StrId::BOOKERLY, StrId::NOTO_SANS, StrId::OPEN_DYSLEXIC}),
    SettingInfo::Enum(StrId::FONT_SIZE, &CrossPointSettings::fontSize, {StrId::SMALL, StrId::MEDIUM, StrId::LARGE, StrId::X_LARGE}),
    SettingInfo::Enum(StrId::LINE_SPACING, &CrossPointSettings::lineSpacing, {StrId::TIGHT, StrId::NORMAL, StrId::WIDE}),
    SettingInfo::Value(StrId::SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum(StrId::PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                      {StrId::JUSTIFY, StrId::ALIGN_LEFT, StrId::CENTER, StrId::ALIGN_RIGHT}),
    SettingInfo::Toggle(StrId::HYPHENATION, &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum(StrId::ORIENTATION, &CrossPointSettings::orientation,
                      {StrId::PORTRAIT, StrId::LANDSCAPE_CW, StrId::INVERTED, StrId::LANDSCAPE_CCW}),
    SettingInfo::Toggle(StrId::EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle(StrId::TEXT_AA, &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    // Launches the remap wizard for front buttons.
    SettingInfo::Action(StrId::REMAP_FRONT_BUTTONS),
    SettingInfo::Enum(StrId::SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                      {StrId::PREV_NEXT, StrId::NEXT_PREV}),
    SettingInfo::Toggle(StrId::LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum(StrId::SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn, {StrId::IGNORE, StrId::SLEEP, StrId::PAGE_TURN})};

constexpr int systemSettingsCount = 6;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum(StrId::TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                      {StrId::MIN_1, StrId::MIN_5, StrId::MIN_10, StrId::MIN_15, StrId::MIN_30}),
    SettingInfo::Action(StrId::LANGUAGE),
    SettingInfo::Action(StrId::KOREADER_SYNC), SettingInfo::Action(StrId::OPDS_BROWSER), SettingInfo::Action(StrId::CLEAR_READING_CACHE),
    SettingInfo::Action(StrId::CHECK_UPDATES)};
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
    if (setting.nameId == StrId::REMAP_FRONT_BUTTONS) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ButtonRemapActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (setting.nameId == StrId::KOREADER_SYNC) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (setting.nameId == StrId::OPDS_BROWSER) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (setting.nameId == StrId::CLEAR_READING_CACHE) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (setting.nameId == StrId::CHECK_UPDATES) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
    } else if (setting.nameId == StrId::LANGUAGE) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new LanguageSelectActivity(renderer, mappedInput, [this] {
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, i18n(SETTINGS_TITLE));

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, [this](int index) { return std::string(I18N.get(settingsList[index].nameId)); },
      nullptr, nullptr,
      [this](int i) {
        const auto& setting = settingsList[i];
        std::string valueText = "";
        if (settingsList[i].type == SettingType::TOGGLE && settingsList[i].valuePtr != nullptr) {
          const bool value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = value ? i18n(STATE_ON) : i18n(STATE_OFF);
        } else if (settingsList[i].type == SettingType::ENUM && settingsList[i].valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(settingsList[i].valuePtr);
          valueText = I18N.get(settingsList[i].enumValues[value]);
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
  const auto labels = mappedInput.mapLabels(i18n(BACK), i18n(TOGGLE), i18n(DIR_UP), i18n(DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
