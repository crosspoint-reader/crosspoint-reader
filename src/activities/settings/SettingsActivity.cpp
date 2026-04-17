#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingActionDispatch.h"
#include "SettingsList.h"
#include "SettingsSubmenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

bool SettingsActivity::isListItemSelectable(int settingIdx) const {
  return settingIdx >= 0 && settingIdx < settingsCount && !(*currentSettings)[settingIdx].isSeparator;
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Build per-category vectors from the shared settings list.
  // addTo tracks the last subcategory per vector and automatically inserts a separator
  // row whenever a setting carries a new subcategory label.
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  submenuData.clear();
  displaySettings.reserve(20);
  readerSettings.reserve(30);
  controlsSettings.reserve(8);
  systemSettings.reserve(20);
  submenuData.reserve(4);

  StrId lastDisplaySub = StrId::STR_NONE_OPT;
  StrId lastReaderSub = StrId::STR_NONE_OPT;
  StrId lastControlsSub = StrId::STR_NONE_OPT;
  StrId lastSystemSub = StrId::STR_NONE_OPT;

  // Shared placement logic — locates submenu target or inserts separator.
  // Returns the vector the caller should push into (either `vec` or a submenu's items).
  auto locateTarget = [this](std::vector<SettingInfo>& vec, StrId& lastSub,
                             const SettingInfo& s) -> std::vector<SettingInfo>* {
    if (s.submenu != StrId::STR_NONE_OPT) {
      auto it = std::find_if(submenuData.begin(), submenuData.end(),
                             [&s](const SubmenuData& d) { return d.id == s.submenu; });
      if (it == submenuData.end()) {
        vec.push_back(SettingInfo::SubmenuEntry(s.submenu));
        submenuData.push_back({s.submenu, {}});
        it = submenuData.end() - 1;
      }
      return &it->items;
    }
    if (s.subcategory != StrId::STR_NONE_OPT && s.subcategory != lastSub) {
      vec.push_back(SettingInfo::Separator(s.subcategory));
      lastSub = s.subcategory;
    }
    return &vec;
  };

  auto addTo = [&locateTarget](std::vector<SettingInfo>& vec, StrId& lastSub, const SettingInfo& s) {
    locateTarget(vec, lastSub, s)->push_back(s);
  };
  auto addToMoved = [&locateTarget](std::vector<SettingInfo>& vec, StrId& lastSub, SettingInfo&& s) {
    auto* target = locateTarget(vec, lastSub, s);
    target->push_back(std::move(s));
  };

  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_SYSTEM &&
        (setting.nameId == StrId::STR_USE_CLOCK || setting.nameId == StrId::STR_CLOCK_FORMAT ||
         setting.nameId == StrId::STR_TIMEZONE)) {
      continue;
    }
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      addTo(displaySettings, lastDisplaySub, setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      addTo(readerSettings, lastReaderSub, setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      addTo(controlsSettings, lastControlsSub, setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      addTo(systemSettings, lastSystemSub, setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Device-only ACTION items — subcategory drives separator insertion automatically.
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));

  addToMoved(readerSettings, lastReaderSub,
             SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  addToMoved(systemSettings, lastSystemSub, SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network)
                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync)
                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser)
                           .withSubcategory(StrId::STR_MENU_SYS_NETWORK)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_CLOCK_SETTINGS, SettingAction::ClockSettings)
                           .withSubcategory(StrId::STR_MENU_SYS_TOOLS)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_WEATHER_SETTINGS, SettingAction::Weather)
                           .withSubcategory(StrId::STR_MENU_SYS_TOOLS)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache)
                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates)
                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));
  addToMoved(systemSettings, lastSystemSub,
             std::move(SettingInfo::Action(StrId::STR_SYSTEM_INFO, SettingAction::SystemInfo)
                           .withSubcategory(StrId::STR_MENU_SYS_SYSTEM)));

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  currentSettings = &displaySettings;
  settingsCount = static_cast<int>(displaySettings.size());

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
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
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1,
                                                      [this](int i) { return i == 0 || isListItemSelectable(i - 1); });
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(
        selectedSettingIndex, settingsCount + 1, [this](int i) { return i == 0 || isListItemSelectable(i - 1); });
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
  if (setting.isSeparator) return;

  if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    if (setting.action == SettingAction::Submenu) {
      auto it = std::find_if(submenuData.begin(), submenuData.end(),
                             [&setting](const SubmenuData& d) { return d.id == setting.nameId; });
      if (it != submenuData.end()) {
        startActivityForResult(
            std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, setting.nameId, it->items), resultHandler);
      }
    } else {
      auto activity = createActivityForAction(setting.action, renderer, mappedInput);
      if (activity) startActivityForResult(std::move(activity), resultHandler);
    }
    return;
  }

  setting.toggleValue();
  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(
      renderer, Rect{contentRect.x, metrics.topPadding + metrics.headerHeight, contentRect.width, metrics.tabBarHeight},
      tabs, selectedSettingIndex == 0);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{contentRect.x, contentTop, contentRect.width,
           contentRect.height -
               (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1, [&settings](int index) { return settings[index].getTitle(); }, nullptr,
      nullptr, [&settings](int i) { return settings[i].getDisplayValue(); }, true);

  // Draw help text
  const auto confirmLabel = (selectedSettingIndex == 0)
                                ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                                : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
