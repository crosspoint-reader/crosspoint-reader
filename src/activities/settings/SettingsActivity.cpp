#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <I18n.h>

#include "CategorySettingsActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int displaySettingsCount = 6;
const SettingInfo displaySettings[displaySettingsCount] = {
    // Should match with SLEEP_SCREEN_MODE
    SettingInfo::Enum(StrId::SLEEP_SCREEN, &CrossPointSettings::sleepScreen, {StrId::DARK, StrId::LIGHT, StrId::CUSTOM, StrId::COVER, StrId::NONE}),
    SettingInfo::Enum(StrId::SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode, {StrId::FIT, StrId::CROP}),
    SettingInfo::Enum(StrId::STATUS_BAR, &CrossPointSettings::statusBar, {StrId::NONE, StrId::NO_PROGRESS, StrId::FULL}),
    SettingInfo::Enum(StrId::HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage, {StrId::NEVER, StrId::IN_READER, StrId::ALWAYS}),
    SettingInfo::Enum(StrId::REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                      {StrId::PAGES_1, StrId::PAGES_5, StrId::PAGES_10, StrId::PAGES_15, StrId::PAGES_30}),
    SettingInfo::Action(StrId::EXT_UI_FONT)};

constexpr int readerSettingsCount = 9;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Action(StrId::EXT_READER_FONT),
    SettingInfo::Enum(StrId::FONT_SIZE, &CrossPointSettings::fontSize, {StrId::SMALL, StrId::MEDIUM, StrId::LARGE, StrId::X_LARGE}),
    SettingInfo::Enum(StrId::LINE_SPACING, &CrossPointSettings::lineSpacing, {StrId::TIGHT, StrId::NORMAL, StrId::WIDE}),
    SettingInfo::Value(StrId::SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}),
    SettingInfo::Enum(StrId::PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                      {StrId::JUSTIFY, StrId::LEFT, StrId::CENTER, StrId::RIGHT}),
    SettingInfo::Toggle(StrId::HYPHENATION, &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum(StrId::ORIENTATION, &CrossPointSettings::orientation,
                      {StrId::PORTRAIT, StrId::LANDSCAPE_CW, StrId::INVERTED, StrId::LANDSCAPE_CCW}),
    SettingInfo::Toggle(StrId::EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle(StrId::TEXT_AA, &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Enum(StrId::FRONT_BTN_LAYOUT, &CrossPointSettings::frontButtonLayout,
                      {StrId::FRONT_LAYOUT_BCLR, StrId::FRONT_LAYOUT_LRBC, StrId::FRONT_LAYOUT_LBCR}),
    SettingInfo::Enum(StrId::SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                      {StrId::PREV_NEXT, StrId::NEXT_PREV}),
    SettingInfo::Toggle(StrId::LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum(StrId::SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn, {StrId::IGNORE, StrId::SLEEP, StrId::PAGE_TURN})};

constexpr int systemSettingsCount = 6;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum(StrId::TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                      {StrId::MIN_1, StrId::MIN_5, StrId::MIN_10, StrId::MIN_15, StrId::MIN_30}),
    SettingInfo::Action(StrId::LANGUAGE),
    SettingInfo::Action(StrId::KOREADER_SYNC), SettingInfo::Action(StrId::CALIBRE_SETTINGS), SettingInfo::Action(StrId::CLEAR_READING_CACHE),
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

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();

  const SettingInfo* settingsList = nullptr;
  int settingsCount = 0;

  // Category StrIds for dynamic translation
  static constexpr StrId categoryStrIds[categoryCount] = {
    StrId::CAT_DISPLAY, StrId::CAT_READER, StrId::CAT_CONTROLS, StrId::CAT_SYSTEM
  };

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

  enterNewActivity(new CategorySettingsActivity(renderer, mappedInput, I18N.get(categoryStrIds[categoryIndex]), settingsList,
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
  renderer.drawCenteredText(UI_12_FONT_ID, 15, TR(SETTINGS_TITLE), true, EpdFontFamily::BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectedCategoryIndex * 30 - 2, pageWidth - 1, 30);

  // Category StrIds for dynamic translation
  static constexpr StrId categoryStrIds[categoryCount] = {
    StrId::CAT_DISPLAY, StrId::CAT_READER, StrId::CAT_CONTROLS, StrId::CAT_SYSTEM
  };

  // Draw all categories
  for (int i = 0; i < categoryCount; i++) {
    const int categoryY = 60 + i * 30;  // 30 pixels between categories

    // Draw category name (dynamically translated)
    renderer.drawText(UI_10_FONT_ID, 20, categoryY, I18N.get(categoryStrIds[i]), i != selectedCategoryIndex);
  }

  // Draw version text above button hints
  renderer.drawText(SMALL_FONT_ID, pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
                    pageHeight - 60, CROSSPOINT_VERSION);

  // Draw help text
  const auto labels = mappedInput.mapLabels(TR(BACK), TR(SELECT), "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
