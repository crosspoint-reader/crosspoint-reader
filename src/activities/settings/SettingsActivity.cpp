#include "SettingsActivity.h"

#include <GfxRenderer.h>

#include "Battery.h"
#include "CalibreSettingsActivity.h"
#include "CategorySettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "ThemeSelectionActivity.h"
#include "fontIds.h"

const char *SettingsActivity::categoryNames[categoryCount] = {
    "Display", "Reader", "Controls", "System"};

namespace {
constexpr int displaySettingsCount = 7;
const SettingInfo displaySettings[displaySettingsCount] = {
    SettingInfo::Enum("Sleep Screen", &CrossPointSettings::sleepScreen, {"Dark", "Light", "Custom", "Cover", "None"}),
    SettingInfo::Enum("Sleep Screen Cover Mode", &CrossPointSettings::sleepScreenCoverMode, {"Fit", "Crop"}),
    SettingInfo::Enum("Sleep Screen Cover Filter", &CrossPointSettings::sleepScreenCoverFilter,
                      {"None", "Contrast", "Inverted"}),
    SettingInfo::Enum("Status Bar", &CrossPointSettings::statusBar,
                      {"None", "No Progress", "Full w/ Percentage", "Full w/ Progress Bar", "Progress Bar"}),
    SettingInfo::Enum("Hide Battery %", &CrossPointSettings::hideBatteryPercentage, {"Never", "In Reader", "Always"}),
    SettingInfo::Enum("Refresh Frequency", &CrossPointSettings::refreshFrequency,
                      {"1 page", "5 pages", "10 pages", "15 pages", "30 pages"}),
    SettingInfo::Action("Theme")};

constexpr int readerSettingsCount = 9;
const SettingInfo readerSettings[readerSettingsCount] = {
    SettingInfo::Enum("Font Family", &CrossPointSettings::fontFamily,
                      {"Bookerly", "Noto Sans", "Open Dyslexic"}),
    SettingInfo::Enum("Font Size", &CrossPointSettings::fontSize,
                      {"Small", "Medium", "Large", "X Large"}),
    SettingInfo::Enum("Line Spacing", &CrossPointSettings::lineSpacing,
                      {"Tight", "Normal", "Wide"}),
    SettingInfo::Value("Screen Margin", &CrossPointSettings::screenMargin,
                       {5, 40, 5}),
    SettingInfo::Enum("Paragraph Alignment",
                      &CrossPointSettings::paragraphAlignment,
                      {"Justify", "Left", "Center", "Right"}),
    SettingInfo::Toggle("Hyphenation", &CrossPointSettings::hyphenationEnabled),
    SettingInfo::Enum(
        "Reading Orientation", &CrossPointSettings::orientation,
        {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"}),
    SettingInfo::Toggle("Extra Paragraph Spacing",
                        &CrossPointSettings::extraParagraphSpacing),
    SettingInfo::Toggle("Text Anti-Aliasing",
                        &CrossPointSettings::textAntiAliasing)};

constexpr int controlsSettingsCount = 4;
const SettingInfo controlsSettings[controlsSettingsCount] = {
    SettingInfo::Enum(
        "Front Button Layout", &CrossPointSettings::frontButtonLayout,
        {"Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght", "Bck, Cnfrm, Rght, Lft"}),
    SettingInfo::Enum("Side Button Layout (reader)", &CrossPointSettings::sideButtonLayout,
                      {"Prev, Next", "Next, Prev"}),
    SettingInfo::Toggle("Long-press Chapter Skip",
                        &CrossPointSettings::longPressChapterSkip),
    SettingInfo::Enum("Short Power Button Click",
                      &CrossPointSettings::shortPwrBtn,
                      {"Ignore", "Sleep", "Page Turn"})};

constexpr int systemSettingsCount = 5;
const SettingInfo systemSettings[systemSettingsCount] = {
    SettingInfo::Enum("Time to Sleep", &CrossPointSettings::sleepTimeout,
                      {"1 min", "5 min", "10 min", "15 min", "30 min"}),
    SettingInfo::Action("KOReader Sync"), SettingInfo::Action("OPDS Browser"), SettingInfo::Action("Clear Cache"),
    SettingInfo::Action("Check for updates")};

// All categories with their settings
struct CategoryData {
  const char* name;
  const SettingInfo* settings;
  int count;
};

const CategoryData allCategories[4] = {
    {"Display", displaySettings, displaySettingsCount},
    {"Reader", readerSettings, readerSettingsCount},
    {"Controls", controlsSettings, controlsSettingsCount},
    {"System", systemSettings, systemSettingsCount}};

void updateContextForSetting(ThemeEngine::ThemeContext& ctx, const std::string& prefix, int i, const SettingInfo& info,
                             bool isSelected, bool fullUpdate) {
  if (fullUpdate) {
    ctx.setListItem(prefix, i, "Name", info.name);
    ctx.setListItem(prefix, i, "Type",
                    info.type == SettingType::TOGGLE   ? "Toggle"
                    : info.type == SettingType::ENUM   ? "Enum"
                    : info.type == SettingType::ACTION ? "Action"
                    : info.type == SettingType::VALUE  ? "Value"
                                                       : "Unknown");
  }
  ctx.setListItem(prefix, i, "Selected", isSelected);

  // Values definitely need update
  if (info.type == SettingType::TOGGLE && info.valuePtr) {
    bool val = SETTINGS.*(info.valuePtr);
    ctx.setListItem(prefix, i, "Value", val);
    ctx.setListItem(prefix, i, "ValueLabel", val ? "On" : "Off");
  } else if (info.type == SettingType::ENUM && info.valuePtr) {
    uint8_t val = SETTINGS.*(info.valuePtr);
    if (val < info.enumValues.size()) {
      ctx.setListItem(prefix, i, "Value", info.enumValues[val]);
      ctx.setListItem(prefix, i, "ValueLabel", info.enumValues[val]);
      ctx.setListItem(prefix, i, "ValueIndex", static_cast<int>(val));
    }
  } else if (info.type == SettingType::VALUE && info.valuePtr) {
    int val = SETTINGS.*(info.valuePtr);
    ctx.setListItem(prefix, i, "Value", val);
    ctx.setListItem(prefix, i, "ValueLabel", std::to_string(val));
  } else if (info.type == SettingType::ACTION) {
    if (fullUpdate) {
      ctx.setListItem(prefix, i, "Value", "");
      ctx.setListItem(prefix, i, "ValueLabel", "");
    }
  }
}
}  // namespace
>>>>>>> e6b3ecc (feat: Enhance ThemeEngine and apply new theming to SettingsActivity)

void SettingsActivity::taskTrampoline(void *param) {
  auto *self = static_cast<SettingsActivity *>(param);
  self->displayTaskLoop();
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // For themed mode, provide all data upfront
  if (ThemeEngine::ThemeManager::get().getElement("Settings")) {
    updateThemeContext(true); // Full update
  }

  updateRequired = true;

  xTaskCreate(&SettingsActivity::taskTrampoline, "SettingsActivityTask",
              4096,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &displayTaskHandle // Task handle
  );
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void SettingsActivity::loop() {
  if (subActivityExitPending) {
    subActivityExitPending = false;
    exitActivity();
    updateThemeContext(true);
    updateRequired = true;
  }

  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (ThemeEngine::ThemeManager::get().getElement("Settings")) {
    handleThemeInput();
    return;
  }

  // Legacy mode
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    enterCategoryLegacy(selectedCategoryIndex);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Move selection up (with wrap-around)
    selectedCategoryIndex = (selectedCategoryIndex > 0)
                                ? (selectedCategoryIndex - 1)
                                : (categoryCount - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Move selection down (with wrap around)
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1)
                                ? (selectedCategoryIndex + 1)
                                : 0;
    updateRequired = true;
  }
}

void SettingsActivity::enterCategoryLegacy(int categoryIndex) {
  if (categoryIndex < 0 || categoryIndex >= categoryCount) return;

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new CategorySettingsActivity(
      renderer, mappedInput, allCategories[categoryIndex].name,
      allCategories[categoryIndex].settings, allCategories[categoryIndex].count,
      [this] {
        exitActivity();
        updateRequired = true;
      }));
  xSemaphoreGive(renderingMutex);
}

void SettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;

      if (xSemaphoreTake(renderingMutex, portMAX_DELAY) == pdTRUE) {
        render();
        xSemaphoreGive(renderingMutex);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void SettingsActivity::render() const {
  renderer.clearScreen();

  if (ThemeEngine::ThemeManager::get().getElement("Settings")) {
    ThemeEngine::ThemeManager::get().renderScreen("Settings", renderer, themeContext);
    renderer.displayBuffer();
    return;
  }

  // Legacy rendering
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Settings", true,
                            EpdFontFamily::BOLD);

  // Draw selection
  renderer.fillRect(0, 60 + selectedCategoryIndex * 30 - 2, pageWidth - 1, 30);

  for (int i = 0; i < categoryCount; i++) {
    const int categoryY = 60 + i * 30; // 30 pixels between categories

    // Draw category name
    renderer.drawText(UI_10_FONT_ID, 20, categoryY, categoryNames[i],
                      i != selectedCategoryIndex);
  }

  // Draw version text above button hints
  renderer.drawText(
      SMALL_FONT_ID,
      pageWidth - 20 - renderer.getTextWidth(SMALL_FONT_ID, CROSSPOINT_VERSION),
      pageHeight - 60, CROSSPOINT_VERSION);

  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3,
                           labels.btn4);

  renderer.displayBuffer();
}

void SettingsActivity::updateThemeContext(bool fullUpdate) {
  themeContext.setInt("System.Battery", battery.readPercentage());

  // Categories
  if (fullUpdate) {
    themeContext.setInt("Categories.Count", categoryCount);
  }
  
  themeContext.setInt("Categories.Selected", selectedCategoryIndex);

  for (int i = 0; i < categoryCount; i++) {
    if (fullUpdate) {
      themeContext.setListItem("Categories", i, "Name", allCategories[i].name);
      themeContext.setListItem("Categories", i, "SettingsCount", allCategories[i].count);
    }
    themeContext.setListItem("Categories", i, "Selected", i == selectedCategoryIndex);
  }

  // Provide ALL settings for ALL categories
  // Format: Category0.Settings.0.Name, Category0.Settings.1.Name, etc.
  for (int cat = 0; cat < categoryCount; cat++) {
    std::string catPrefix = "Category" + std::to_string(cat) + ".Settings";
    for (int i = 0; i < allCategories[cat].count; i++) {
      bool isSelected = (cat == selectedCategoryIndex && i == selectedSettingIndex);
      updateContextForSetting(themeContext, catPrefix, i, allCategories[cat].settings[i], isSelected, fullUpdate);
    }
  }

  // Also provide current category's settings as "Settings" for simpler themes
  if (fullUpdate) {
     themeContext.setInt("Settings.Count", allCategories[selectedCategoryIndex].count);
  }
  
  for (int i = 0; i < allCategories[selectedCategoryIndex].count; i++) {
    updateContextForSetting(themeContext, "Settings", i, allCategories[selectedCategoryIndex].settings[i],
                            i == selectedSettingIndex, fullUpdate);
  }
}

void SettingsActivity::handleThemeInput() {
  const int currentCategorySettingsCount = allCategories[selectedCategoryIndex].count;

  // Up/Down navigates settings within current category
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedSettingIndex = (selectedSettingIndex > 0) ? (selectedSettingIndex - 1) : (currentCategorySettingsCount - 1);
    updateThemeContext(false); // Partial update
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedSettingIndex = (selectedSettingIndex < currentCategorySettingsCount - 1) ? (selectedSettingIndex + 1) : 0;
    updateThemeContext(false); // Partial update
    updateRequired = true;
    return;
  }

  // Left/Right/PageBack/PageForward switches categories
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    selectedCategoryIndex = (selectedCategoryIndex > 0) ? (selectedCategoryIndex - 1) : (categoryCount - 1);
    selectedSettingIndex = 0;  // Reset to first setting in new category
    updateThemeContext(true); // Full update (category changed)
    updateRequired = true;
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
    selectedSettingIndex = 0;
    updateThemeContext(true); // Full update
    updateRequired = true;
    return;
  }

  // Confirm toggles/activates current setting
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateThemeContext(false); // Values changed, partial update is enough (names don't change)
    updateRequired = true;
    return;
  }

  // Back exits
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
  }
}

void SettingsActivity::toggleCurrentSetting() {
  const auto& setting = allCategories[selectedCategoryIndex].settings[selectedSettingIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    bool currentVal = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentVal;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr) {
    uint8_t val = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (val + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr) {
    int8_t val = SETTINGS.*(setting.valuePtr);
    if (val + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = val + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    if (strcmp(setting.name, "KOReader Sync") == 0) {
      enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, [this] { subActivityExitPending = true; }));
    } else if (strcmp(setting.name, "Calibre Settings") == 0) {
      enterNewActivity(new CalibreSettingsActivity(renderer, mappedInput, [this] { subActivityExitPending = true; }));
    } else if (strcmp(setting.name, "Clear Cache") == 0) {
      enterNewActivity(new ClearCacheActivity(renderer, mappedInput, [this] { subActivityExitPending = true; }));
    } else if (strcmp(setting.name, "Check for updates") == 0) {
      enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, [this] { subActivityExitPending = true; }));
    } else if (strcmp(setting.name, "Theme") == 0) {
      enterNewActivity(new ThemeSelectionActivity(renderer, mappedInput, [this] { subActivityExitPending = true; }));
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }

  SETTINGS.saveToFile();
}
