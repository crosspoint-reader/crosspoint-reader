#include "HomeButtonSettingsActivity.h"

#include <utility>

#include "components/UITheme.h"

namespace fui = freeink::ui;

bool HomeButtonSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void HomeButtonSettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount() || optionPopup.isActive()) return;
  mappedInput.resetHomeButtonInput();
  app.clearTapFlash();
  nav.selected = index;
  const auto field = home_button::FIELDS[index];
  const uint8_t value = SETTINGS.*field;
  const int current = value < static_cast<uint8_t>(HomeButtonAction::Count) ? value : 0;
  optionPopup.show(home_button::GESTURE_LABELS[index], home_button::ACTION_LABELS,
                   static_cast<int>(HomeButtonAction::Count), current, [field](int selected) {
                     SETTINGS.*field = static_cast<uint8_t>(selected);
                     SETTINGS.saveToFile();
                   });
  requestUpdate();
}

void HomeButtonSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void HomeButtonSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  for (int i = 0; i < listCount(); ++i) {
    rows[i].label = I18N.get(home_button::GESTURE_LABELS[i]);
    rows[i].actionValue = static_cast<int16_t>(i);
    const uint8_t value = SETTINGS.*home_button::FIELDS[i];
    rows[i].value = value < static_cast<uint8_t>(HomeButtonAction::Count) ? I18N.get(home_button::ACTION_LABELS[value])
                                                                          : I18N.get(home_button::ACTION_LABELS[0]);
  }
  fui::ListProps props;
  props.items = rows;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // Keep the gesture name and its current action at the same visual weight.
  props.labelText = screen.theme().smallText;
  // A default smallText style is treated as inherited by screen.list().
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
