#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Start on requested category
  selectedCategoryIndex = initialCategory;
  selectedSettingIndex = 0;

  switch (selectedCategoryIndex) {
    case 1:  currentSettings = &readerSettings;   settingsCount = static_cast<int>(readerSettings.size());   break;
    case 2:  currentSettings = &controlsSettings; settingsCount = static_cast<int>(controlsSettings.size()); break;
    case 3:  currentSettings = &systemSettings;   settingsCount = static_cast<int>(systemSettings.size());   break;
    default: currentSettings = &displaySettings;  settingsCount = static_cast<int>(displaySettings.size());  break;
  }

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (selectedSettingIndex == 0) {
    // Focus on category tiles — grid navigation
    bool catChanged = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedCategoryIndex ^= 1;  // toggle column
      catChanged = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedCategoryIndex ^= 2;  // toggle row
      catChanged = true;
    }
    if (catChanged) {
      switch (selectedCategoryIndex) {
        case 0: currentSettings = &displaySettings;  break;
        case 1: currentSettings = &readerSettings;   break;
        case 2: currentSettings = &controlsSettings; break;
        case 3: currentSettings = &systemSettings;   break;
      }
      settingsCount = static_cast<int>(currentSettings->size());
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (settingsCount > 0) { selectedSettingIndex = 1; requestUpdate(); }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      SETTINGS.saveToFile();
      finish();
    }
  } else {
    // Focus on settings list
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (selectedSettingIndex < settingsCount) { selectedSettingIndex++; requestUpdate(); }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (selectedSettingIndex > 1) { selectedSettingIndex--; requestUpdate(); }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      selectedSettingIndex = 0;
      requestUpdate();
    }
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

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
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<CalibreSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  constexpr int pad = 14;
  constexpr int headerH = 36;
  constexpr int tileH = 52;
  constexpr int tileGap = 5;
  constexpr int tilesH = tileH * 2 + tileGap + 10;
  constexpr int dividerY = headerH + tilesH;
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header
  renderer.drawText(UI_12_FONT_ID, pad, 8, tr(STR_SETTINGS_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawLine(pad, headerH - 2, W - pad, headerH - 2, true);

  // 2×2 category tiles
  static const char* catLabels[4] = {"DISPLAY", "READER", "CONTROLS", "SYSTEM"};
  const int tileW = (W - pad * 2 - tileGap) / 2;
  for (int i = 0; i < categoryCount; i++) {
    int col = i % 2;
    int row = i / 2;
    int tx = pad + col * (tileW + tileGap);
    int ty = headerH + 5 + row * (tileH + tileGap);
    bool isSel = (i == selectedCategoryIndex);
    if (isSel) renderer.fillRect(tx, ty, tileW, tileH, true);
    else        renderer.drawRect(tx, ty, tileW, tileH, true);
    int lw = renderer.getTextWidth(SMALL_FONT_ID, catLabels[i]);
    int lh = renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, tx + (tileW - lw) / 2, ty + (tileH - lh) / 2, catLabels[i], !isSel);
  }

  // Separator between tiles and settings list
  renderer.drawLine(pad, dividerY, W - pad, dividerY, true);

  // Settings list
  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, dividerY + 4, W, H - dividerY - 4 - metrics.buttonHintsHeight},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); },
      nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText;
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          valueText = SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          valueText = I18N.get(setting.enumValues[SETTINGS.*(setting.valuePtr)]);
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        }
        return valueText;
      },
      selectedSettingIndex > 0);

  // Button hints
  const char* confirmLabel = (selectedSettingIndex == 0) ? tr(STR_SELECT) : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
