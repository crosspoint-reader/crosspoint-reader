#include "KeyboardLayoutsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void KeyboardLayoutsActivity::onEnter() {
  UiListActivity::onEnter();
  // Start from the effective set, not the raw setting: an unconfigured mask
  // shows as the derived default (UI language + English) rather than as nothing
  // ticked, so the screen reflects what the keyboard actually offers.
  workingMask = keyboard_layouts::enabled();
  edited = false;

  // Labels never change while the screen is open, so they're set once here
  // rather than on every buildScreen() call. The layout's name is its
  // language's name, so adding a layout needs no new translation keys.
  for (int i = 0; i < totalItems; ++i) {
    rowItems[i].label = I18N.getLanguageName(keyboard_layouts::ALL[i].language);
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

void KeyboardLayoutsActivity::onExit() {
  if (edited && workingMask != SETTINGS.keyboardLayouts) {
    SETTINGS.keyboardLayouts = workingMask;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

const char* KeyboardLayoutsActivity::headerTitle() const { return tr(STR_KEYBOARD_LAYOUTS); }

void KeyboardLayoutsActivity::activateIndex(const int index) {
  nav.selected = index;
  // The row stays on screen with a new ON/OFF value; a lingering flash would
  // gray an unrelated row on the repaint below.
  app.clearTapFlash();

  const uint16_t bit = keyboard_layouts::layoutBit(keyboard_layouts::ALL[index].id);
  const bool wasOn = (workingMask & bit) != 0;
  // Refuse to switch off the last one: an empty set would leave the keyboard
  // with no letters at all.
  if (wasOn && (workingMask & ~bit) == 0) return;
  workingMask = static_cast<uint16_t>(wasOn ? (workingMask & ~bit) : (workingMask | bit));
  edited = true;
  requestUpdate();
}

void KeyboardLayoutsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Labels/actionValue were set in onEnter(); only the toggle state is live.
  // tr() returns a pointer into the I18n string table, so nothing is stored.
  for (int i = 0; i < totalItems; ++i) {
    rowItems[i].value =
        (workingMask & keyboard_layouts::layoutBit(keyboard_layouts::ALL[i].id)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(totalItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
