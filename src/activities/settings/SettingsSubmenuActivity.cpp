#include "SettingsSubmenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "StatusBarSettingsActivity.h"
#include "SystemInformationActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/weather/WeatherSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsSubmenuActivity::onEnter() {
  Activity::onEnter();
  itemCount = static_cast<int>(items.size());
  const auto pred = UITheme::makeSelectablePredicate(itemCount, [this](int i) { return items[i].getTitle(); });
  buttonNavigator.setSelectablePredicate(pred, itemCount);
  if (!pred(selectedIndex)) {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
  }
  requestUpdate();
}

void SettingsSubmenuActivity::onExit() { Activity::onExit(); }

void SettingsSubmenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleItem();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });
}

void SettingsSubmenuActivity::toggleItem() {
  if (selectedIndex < 0 || selectedIndex >= itemCount) return;
  const auto& setting = items[selectedIndex];
  if (setting.isSeparator) return;

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (cur + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (cur + setting.valueRange.step > setting.valueRange.max)
                                       ? setting.valueRange.min
                                       : cur + setting.valueRange.step;
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };
    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::ClockSettings:
        startActivityForResult(std::make_unique<ClockSettingsActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<CalibreSettingsActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        return;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::Weather:
        startActivityForResult(std::make_unique<WeatherSettingsActivity>(renderer, mappedInput), resultHandler);
        return;
      case SettingAction::SystemInfo:
        startActivityForResult(std::make_unique<SystemInformationActivity>(renderer, mappedInput), resultHandler);
        return;
      default:
        return;
    }
  }

  SETTINGS.saveToFile();
}

void SettingsSubmenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(titleId));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, itemCount, selectedIndex,
      [this](int index) { return items[index].getTitle(); }, nullptr, nullptr,
      [this](int i) {
        const auto& setting = items[i];
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          return std::string(SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
        }
        if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          if (value < setting.enumValues.size()) {
            return std::string(I18N.get(setting.enumValues[value]));
          }
        }
        if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          return std::to_string(SETTINGS.*(setting.valuePtr));
        }
        if (setting.type == SettingType::ACTION && !setting.isSeparator) {
          return std::string(">>");
        }
        return std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
