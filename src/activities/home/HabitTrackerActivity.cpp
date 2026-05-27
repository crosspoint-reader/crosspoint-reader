#include "HabitTrackerActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void HabitTrackerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate(true);
}

void HabitTrackerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::HABIT_TRACKER);
    return;
  }

  // Stub screen: keep button behavior consistent with other menu screens.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    requestUpdate();
  }
}

void HabitTrackerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textTop = (renderer.getScreenHeight() - textHeight) / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, textTop, tr(STR_HABIT_TRACKER), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
