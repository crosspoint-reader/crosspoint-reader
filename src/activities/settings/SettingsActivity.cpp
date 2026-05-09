#include "SettingsActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
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
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // Insert "Download Fonts" right after the font family setting so users discover it naturally
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_DOWNLOAD_FONTS, SettingAction::DownloadFonts));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (coverPopupVisible) {
    handleCoverPopup();
    return;
  }

  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
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
    if (setting.nameId == StrId::STR_HOME_COVER) {
      coverModeBeforePopup = SETTINGS.coverMode;
      switch (SETTINGS.coverMode) {
        case CrossPointSettings::COVER_ENABLED:
          coverPopupSelection = 0;
          break;
        case CrossPointSettings::COVER_TIMEOUT:
          coverPopupSelection = 2;
          break;
        case CrossPointSettings::COVER_DISABLED:
          coverPopupSelection = 3;
          break;
        default:
          coverPopupSelection = 0;
          break;
      }
      coverPopupVisible = true;
      requestUpdate();
      return;
    }

    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
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
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
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
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel = (selectedSettingIndex == 0)
                                ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                                : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (coverPopupVisible) {
    renderCoverPopup();
  }
  renderer.displayBuffer();
}

void SettingsActivity::handleCoverPopup() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    coverPopupSelection = (coverPopupSelection - 1 + kCoverOptionCount) % kCoverOptionCount;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    coverPopupSelection = (coverPopupSelection + 1) % kCoverOptionCount;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    applyCoverOption();
    coverPopupVisible = false;
    SETTINGS.saveToFile();
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.coverMode = coverModeBeforePopup;
    coverPopupVisible = false;
    requestUpdate();
  }
}

void SettingsActivity::applyCoverOption() {
  switch (coverPopupSelection) {
    case 0:  // Enabled
      SETTINGS.coverMode = CrossPointSettings::COVER_ENABLED;
      resetAllCoverDisabled();
      break;
    case 1:  // Regenerate All
      SETTINGS.coverMode = CrossPointSettings::COVER_ENABLED;
      resetAllCoverDisabled();
      APP_STATE.forceRenderCoverPath = "__regenerate_all__";
      break;
    case 2:  // Timeout
      SETTINGS.coverMode = CrossPointSettings::COVER_TIMEOUT;
      resetAllCoverDisabled();
      break;
    case 3:  // Disabled
      SETTINGS.coverMode = CrossPointSettings::COVER_DISABLED;
      break;
    case 4:  // Delete All
      SETTINGS.coverMode = CrossPointSettings::COVER_DISABLED;
      deleteAllCoverThumbs();
      break;
  }
}

void SettingsActivity::deleteAllCoverThumbs() {
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  char name[128];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    if (file.isDirectory() && itemName.startsWith("epub_")) {
      file.close();
      String dirPath = "/.crosspoint/" + itemName;
      auto dir = Storage.open(dirPath.c_str());
      if (dir && dir.isDirectory()) {
        char entryName[64];
        for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
          entry.getName(entryName, sizeof(entryName));
          String eName(entryName);
          if (!entry.isDirectory() && eName.startsWith("thumb_") && eName.endsWith(".bmp")) {
            String thumbPath = dirPath + "/" + eName;
            Storage.remove(thumbPath.c_str());
          }
          entry.close();
        }
      }
      if (dir) dir.close();
    } else {
      file.close();
    }
  }
  root.close();

  const auto& books = RECENT_BOOKS.getBooks();
  for (const auto& book : books) {
    if (FsHelpers::hasEpubExtension(book.path)) {
      RECENT_BOOKS.setCoverDisabled(book.path, true);
    }
  }
}

void SettingsActivity::resetAllCoverDisabled() {
  const auto& books = RECENT_BOOKS.getBooks();
  for (const auto& book : books) {
    RECENT_BOOKS.setCoverDisabled(book.path, false);
  }
}

void SettingsActivity::renderCoverPopup() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height10 = renderer.getLineHeight(UI_10_FONT_ID);
  const auto height12 = renderer.getLineHeight(UI_12_FONT_ID);
  const auto heightSmall = renderer.getLineHeight(SMALL_FONT_ID);

  static const StrId optionLabels[kCoverOptionCount] = {
      StrId::STR_HOME_COVER_ENABLED, StrId::STR_HOME_COVER_REGENERATE_ALL, StrId::STR_HOME_COVER_TIMEOUT,
      StrId::STR_HOME_COVER_DISABLED, StrId::STR_HOME_COVER_DELETE_ALL};

  static const StrId optionDescs[kCoverOptionCount] = {
      StrId::STR_HOME_COVER_ENABLED_DESC, StrId::STR_HOME_COVER_REGENERATE_ALL_DESC, StrId::STR_HOME_COVER_TIMEOUT_DESC,
      StrId::STR_HOME_COVER_DISABLED_DESC, StrId::STR_HOME_COVER_DELETE_ALL_DESC};

  constexpr int itemSpacing = 6;
  constexpr int innerPadding = 16;

  int maxTextWidth = 0;
  for (int i = 0; i < kCoverOptionCount; i++) {
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, I18N.get(optionLabels[i]), EpdFontFamily::BOLD);
    if (labelW > maxTextWidth) maxTextWidth = labelW;
    const int descW = renderer.getTextWidth(SMALL_FONT_ID, I18N.get(optionDescs[i]));
    if (descW > maxTextWidth) maxTextWidth = descW;
  }

  const int listHeight = height10 * kCoverOptionCount + itemSpacing * (kCoverOptionCount - 1);
  const int dialogW = std::min((maxTextWidth + innerPadding * 2) * 12 / 10, pageWidth - 20);
  const int contentHeight = height12 + 10 + listHeight + 14 + heightSmall + 14 + height10;
  const int dialogH = contentHeight + innerPadding * 2;
  const int dialogX = (pageWidth - dialogW) / 2;
  const int dialogY = (pageHeight - dialogH) / 2;

  GUI.drawDialogBackground(renderer, Rect{dialogX, dialogY, dialogW, dialogH});

  int y = dialogY + innerPadding;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_HOME_COVER), true, EpdFontFamily::BOLD);
  y += height12 + 10;

  constexpr int selectionHPadding = 8;
  constexpr int selectionVPadding = 4;
  for (int i = 0; i < kCoverOptionCount; i++) {
    const int itemY = y + i * (height10 + itemSpacing);
    const bool selected = (i == coverPopupSelection);
    const char* labelText = I18N.get(optionLabels[i]);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, labelText, EpdFontFamily::BOLD);
    const int labelX = (pageWidth - labelWidth) / 2;

    Rect itemRect(labelX - selectionHPadding, itemY - selectionVPadding, labelWidth + selectionHPadding * 2,
                  height10 + selectionVPadding * 2);
    GUI.drawPopupSelection(renderer, UI_10_FONT_ID, itemRect, labelText, selected);
  }

  y += listHeight + 14;
  renderer.drawCenteredText(SMALL_FONT_ID, y, I18N.get(optionDescs[coverPopupSelection]), true);

  y = dialogY + dialogH - innerPadding - height10;
  const int backWidth = renderer.getTextWidth(UI_10_FONT_ID, "[BACK]");
  const int selectWidth = renderer.getTextWidth(UI_10_FONT_ID, "[SELECT]");
  const int hintsWidth = backWidth + 30 + selectWidth;
  int hintsX = (pageWidth - hintsWidth) / 2;
  renderer.drawText(UI_10_FONT_ID, hintsX, y, "[BACK]", true);
  renderer.drawText(UI_10_FONT_ID, hintsX + backWidth + 30, y, "[SELECT]", true);
}
