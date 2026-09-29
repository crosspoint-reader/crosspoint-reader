#include "ControlsSubmenuActivity.h"

#include <utility>

#include "ButtonRemapActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

ControlsSubmenuActivity::ControlsSubmenuActivity(GfxRenderer& renderer, MappedInputManager& input, StrId title,
                                                 std::vector<SettingInfo> rows)
    : UiListActivity("ControlsSubmenu", renderer, input), title_(title), rows_(std::move(rows)) {
  visible_.reserve(rows_.size());
  items_.reserve(rows_.size());
  values_.reserve(rows_.size());
  rebuildVisible();
}

void ControlsSubmenuActivity::rebuildVisible() {
  visible_.clear();
  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    // The footnote-back toggle only applies while the power click action is
    // Footnotes.
    const bool hidden = rows_[i].valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
                        SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES;
    // cppcheck-suppress useStlAlgorithm
    if (!hidden) visible_.push_back(i);
  }
}

bool ControlsSubmenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ControlsSubmenuActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount() || optionPopup.isActive()) return;
  mappedInput.resetHomeButtonInput();
  app.clearTapFlash();
  nav.selected = index;
  const auto& setting = rows_[visible_[index]];

  if (setting.type == SettingType::ACTION) {
    if (setting.action == SettingAction::RemapFrontButtons) {
      startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               requestUpdate();
                             });
    }
    return;
  }
  if (setting.valuePtr == nullptr) return;

  if (setting.type == SettingType::TOGGLE) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
  } else if (setting.type == SettingType::ENUM) {
    const auto labels = setting.enumLabels();
    const uint8_t current = SETTINGS.*(setting.valuePtr);
    if (labels.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, labels.data(), static_cast<int>(labels.size()),
                       current < labels.size() ? current : 0, [valuePtr](int selected) {
                         SETTINGS.*valuePtr = static_cast<uint8_t>(selected);
                         SETTINGS.saveToFile();
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (current + 1) % static_cast<uint8_t>(labels.size());
  } else {
    return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void ControlsSubmenuActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ControlsSubmenuActivity::buildScreen(UiScreen& screen) {
  // Row visibility can depend on other settings (e.g. the footnote-back toggle
  // follows the power click action), so refresh it on every repaint.
  rebuildVisible();
  if (nav.selected >= listCount()) nav.selected = listCount() > 0 ? listCount() - 1 : 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  items_.clear();
  values_.assign(visible_.size(), std::string());
  for (int i = 0; i < listCount(); ++i) {
    const auto& setting = rows_[visible_[i]];
    fui::ListItem item;
    item.label = I18N.get(setting.nameId);
    item.actionValue = static_cast<int16_t>(i);
    values_[i] = SettingsActivity::settingValueText(setting);
    item.value = values_[i].empty() ? nullptr : values_[i].c_str();
    items_.push_back(item);
  }

  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // Keep the row name and its current value at the same visual weight.
  props.labelText = screen.theme().smallText;
  // A default smallText style is treated as inherited by screen.list().
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
