#include "BookReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PerBookReaderSettingsBridge.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "activities/settings/FontSelectionActivity.h"
#include "components/UITheme.h"

BookReaderSettingsActivity::BookReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       PerBookReaderSettings globalDefaults,
                                                       PerBookReaderSettings bookSettings)
    : Activity("BookReaderSettings", renderer, mappedInput),
      globalDefaults(std::move(globalDefaults)),
      savedCustom(std::move(bookSettings)),
      customEnabled(savedCustom.hasReaderOverrides) {}

void BookReaderSettingsActivity::rebuildSettings() {
  settings.clear();
  sdFontSystem.refreshIfDirty();
  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_CAT_READER) settings.push_back(std::move(setting));
  }
}

void BookReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  rebuildSettings();
  requestUpdate();
}

void BookReaderSettingsActivity::setCustomEnabled(const bool enabled) {
  if (enabled == customEnabled) return;
  if (!enabled) {
    savedCustom = captureReaderSettings(true, savedCustom.hasAutoPageTurnInterval, savedCustom.autoPageTurnSeconds,
                                        savedCustom.autoPageTurnStartsOnOpen);
    applyReaderSettings(globalDefaults);
  } else {
    applyReaderSettings(savedCustom);
  }
  customEnabled = enabled;
  sdFontSystem.ensureLoaded(renderer, false);
}

void BookReaderSettingsActivity::finishWithResult() {
  if (customEnabled) {
    savedCustom = captureReaderSettings(true, savedCustom.hasAutoPageTurnInterval, savedCustom.autoPageTurnSeconds,
                                        savedCustom.autoPageTurnStartsOnOpen);
  }
  savedCustom.hasReaderOverrides = customEnabled;
  setResult(ReaderSettingsResult{savedCustom});
  finish();
}

void BookReaderSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const int rowCount = static_cast<int>(settings.size()) + 2;
  buttonNavigator.onNextRelease([this, rowCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount);
    resetConfirmationPending = false;
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, rowCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount);
    resetConfirmationPending = false;
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (resetConfirmationPending) {
      resetConfirmationPending = false;
      requestUpdate();
      return;
    }
    finishWithResult();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) toggleSelected();
}

void BookReaderSettingsActivity::toggleSelected() {
  if (selectedIndex == 0) {
    setCustomEnabled(!customEnabled);
    requestUpdate();
    return;
  }
  if (selectedIndex == static_cast<int>(settings.size()) + 1) {
    if (!resetConfirmationPending) {
      resetConfirmationPending = true;
      requestUpdate();
      return;
    }
    resetConfirmationPending = false;
    applyReaderSettings(globalDefaults);
    // Reset means remove the complete book profile, including its independent
    // auto-page-turn interval. Merely disabling custom typography preserves the
    // snapshot; Reset is the explicit destructive action.
    savedCustom = captureReaderSettings(false, false, 0);
    customEnabled = false;
    sdFontSystem.ensureLoaded(renderer, false);
    requestUpdate();
    return;
  }

  const SettingInfo& setting = settings[selectedIndex - 1];
  if (setting.nameId == StrId::STR_FONT_FAMILY) {
    const uint8_t previousFontFamily = SETTINGS.fontFamily;
    const std::string previousSdFontFamily = SETTINGS.sdFontFamilyName;
    startActivityForResult(
        std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry(), false),
        [this, previousFontFamily, previousSdFontFamily](const ActivityResult&) {
          if (SETTINGS.fontFamily == previousFontFamily && SETTINGS.sdFontFamilyName == previousSdFontFamily) {
            requestUpdate();
            return;
          }

          const uint8_t selectedFontFamily = SETTINGS.fontFamily;
          const std::string selectedSdFontFamily = SETTINGS.sdFontFamilyName;
          if (!customEnabled) setCustomEnabled(true);
          SETTINGS.fontFamily = selectedFontFamily;
          std::strncpy(SETTINGS.sdFontFamilyName, selectedSdFontFamily.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
          SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
          sdFontSystem.ensureLoaded(renderer, false);
          customEnabled = true;
          savedCustom = captureReaderSettings(true, savedCustom.hasAutoPageTurnInterval,
                                              savedCustom.autoPageTurnSeconds, savedCustom.autoPageTurnStartsOnOpen);
          requestUpdate();
        });
    return;
  }

  if (setting.type == SettingType::ENUM && setting.valuePtr && setting.enumValues.size() > 2) {
    const auto valuePtr = setting.valuePtr;
    const uint8_t current = SETTINGS.*valuePtr;
    optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), current,
                     [this, valuePtr](const int value) {
                       if (!customEnabled) setCustomEnabled(true);
                       SETTINGS.*valuePtr = static_cast<uint8_t>(value);
                       savedCustom =
                           captureReaderSettings(true, savedCustom.hasAutoPageTurnInterval,
                                                 savedCustom.autoPageTurnSeconds, savedCustom.autoPageTurnStartsOnOpen);
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (!customEnabled) setCustomEnabled(true);

  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
  } else if (setting.type == SettingType::ENUM && setting.valuePtr) {
    const uint8_t current = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>((current + 1) % setting.enumValues.size());
  } else if (setting.type == SettingType::VALUE && setting.valuePtr) {
    const uint8_t current = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = current + setting.valueRange.step > setting.valueRange.max
                                       ? setting.valueRange.min
                                       : current + setting.valueRange.step;
  }
  savedCustom = captureReaderSettings(true, savedCustom.hasAutoPageTurnInterval, savedCustom.autoPageTurnSeconds,
                                      savedCustom.autoPageTurnStartsOnOpen);
  requestUpdate();
}

std::string BookReaderSettingsActivity::valueForRow(const int index) const {
  if (index == 0) return customEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  if (index == static_cast<int>(settings.size()) + 1) {
    return resetConfirmationPending ? tr(STR_CONFIRM) : std::string{};
  }

  const SettingInfo& setting = settings[index - 1];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr) {
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  if (setting.type == SettingType::ENUM) {
    const uint8_t value = setting.valueGetter ? setting.valueGetter() : SETTINGS.*(setting.valuePtr);
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) return I18N.get(setting.enumValues[value]);
  }
  return {};
}

void BookReaderSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height;

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOK_READER_SETTINGS));
  const int rowCount = static_cast<int>(settings.size()) + 2;
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)}, rowCount,
      selectedIndex,
      [this](const int index) -> std::string {
        if (index == 0) return tr(STR_USE_BOOK_SETTINGS);
        if (index == static_cast<int>(settings.size()) + 1) return tr(STR_RESET_BOOK_SETTINGS);
        return I18N.get(settings[index - 1].nameId);
      },
      nullptr, nullptr, [this](const int index) { return valueForRow(index); }, true,
      [this](const int index) { return !customEnabled && index > 0 && index <= static_cast<int>(settings.size()); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
