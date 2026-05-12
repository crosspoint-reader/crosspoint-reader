#include "ReaderOptionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReaderOptionsActivity::onEnter() {
  Activity::onEnter();
  settings.clear();
  const auto allSettings = getSettingsList();
  settings.reserve(allSettings.size());
  std::copy_if(allSettings.begin(), allSettings.end(), std::back_inserter(settings),
               [](const auto& s) { return s.category == StrId::STR_CAT_READER && s.type != SettingType::ACTION; });
  settingsCount = static_cast<int>(settings.size());
  selectedIndex = 0;
  requestUpdate();
}

void ReaderOptionsActivity::onExit() { Activity::onExit(); }

void ReaderOptionsActivity::toggleCurrentSetting() {
  if (selectedIndex < 0 || selectedIndex >= settingsCount) return;
  const auto& setting = settings[selectedIndex];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    SETTINGS.*(setting.valuePtr) = SETTINGS.*(setting.valuePtr) ? 0 : 1;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t cur = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (cur + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t cur = SETTINGS.*(setting.valuePtr);
    if (cur + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = cur + setting.valueRange.step;
    }
  }
}

void ReaderOptionsActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, settingsCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, settingsCount);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    finish();
    return;
  }
}

void ReaderOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_READER_OPTIONS), nullptr);

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight + metrics.verticalSpacing * 2)},
      settingsCount, selectedIndex,
      [this](int i) { return std::string(I18N.get(settings[i].nameId)); },
      nullptr, nullptr,
      [this](int i) {
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
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
