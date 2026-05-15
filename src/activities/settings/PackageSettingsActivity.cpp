#include "PackageSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

PackageSettingsActivity::PackageSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 Marginalia::PackageManifest package)
    : Activity("PackageSettings", renderer, mappedInput), package_(std::move(package)) {}

void PackageSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  requestUpdate();
}

void PackageSettingsActivity::onExit() { Activity::onExit(); }

void PackageSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int settingCount = static_cast<int>(package_.settings.size());
  if (settingCount <= 0) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);

  buttonNavigator_.onNextRelease([this, settingCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, settingCount);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, settingCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, settingCount);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, settingCount, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, settingCount, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, settingCount, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, settingCount, pageItems);
    requestUpdate();
  });
}

std::string PackageSettingsActivity::settingValueLabel(const Marginalia::PackageSettingDefinition& setting) const {
  if (setting.type == Marginalia::PackageSettingType::Boolean) {
    return Marginalia::readPackageSettingBool(package_.id, setting.id, setting.defaultBool) ? tr(STR_ENABLED)
                                                                                            : tr(STR_DISABLED);
  }

  std::string value = Marginalia::readPackageSettingString(package_.id, setting.id, setting.defaultString);
  if (std::find(setting.options.begin(), setting.options.end(), value) == setting.options.end()) {
    value = setting.defaultString;
  }
  return value;
}

void PackageSettingsActivity::handleSelection() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(package_.settings.size())) return;

  const auto& setting = package_.settings[selectedIndex_];
  if (setting.type == Marginalia::PackageSettingType::Boolean) {
    const bool current = Marginalia::readPackageSettingBool(package_.id, setting.id, setting.defaultBool);
    Marginalia::writePackageSettingBool(package_.id, setting.id, !current);
    return;
  }

  if (setting.options.empty()) return;

  std::string current = Marginalia::readPackageSettingString(package_.id, setting.id, setting.defaultString);
  auto currentOption = std::find(setting.options.begin(), setting.options.end(), current);
  if (currentOption == setting.options.end()) {
    currentOption = setting.options.begin();
  } else {
    ++currentOption;
  }
  if (currentOption == setting.options.end()) {
    currentOption = setting.options.begin();
  }

  Marginalia::writePackageSettingString(package_.id, setting.id, *currentOption);
}

void PackageSettingsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 package_.name.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (package_.settings.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_EXTENSION_SETTINGS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(package_.settings.size()),
        selectedIndex_, [this](int index) { return package_.settings[index].label; }, nullptr, nullptr,
        [this](int index) { return settingValueLabel(package_.settings[index]); }, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
