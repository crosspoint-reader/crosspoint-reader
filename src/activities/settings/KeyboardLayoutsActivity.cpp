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
  // The effective set, not the raw setting: an unconfigured mask shows as the
  // derived default rather than as nothing ticked.
  workingMask = keyboard_layouts::enabled();
  edited = false;

  // Labels never change while the screen is open; only the ON/OFF value does.
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

bool KeyboardLayoutsActivity::isLocked(const uint8_t i) const {
  const uint16_t bit = keyboard_layouts::bitAt(i);
  if (!(workingMask & bit)) return false;
  // An empty set leaves the keyboard with no letters, a Latin-free one cannot
  // type a Wi-Fi passphrase.
  const uint16_t without = static_cast<uint16_t>(workingMask & ~bit);
  return without == 0 || !keyboard_layouts::hasLatin(without);
}

void KeyboardLayoutsActivity::activateIndex(const int index) {
  nav.selected = index;
  // The row stays on screen with a new ON/OFF value; a lingering flash would
  // gray an unrelated row on the repaint below.
  app.clearTapFlash();

  // Locked rows stay on. Repaint anyway -- the tap moved nav.selected and
  // clearTapFlash() suppressed the list's own repaint.
  if (isLocked(static_cast<uint8_t>(index))) {
    requestUpdate();
    return;
  }

  const uint16_t bit = keyboard_layouts::bitAt(static_cast<uint8_t>(index));
  const bool wasOn = (workingMask & bit) != 0;
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

  // tr() returns a pointer into the I18n table, so nothing is stored here.
  // A locked row reads "Default" rather than "On": it does not respond to a
  // tap, and an ON that refuses to turn off reads as a bug.
  for (int i = 0; i < totalItems; ++i) {
    const uint8_t row = static_cast<uint8_t>(i);
    if (isLocked(row)) {
      rowItems[i].value = tr(STR_DEFAULT_VALUE);
    } else {
      rowItems[i].value = (workingMask & keyboard_layouts::bitAt(row)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    }
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(totalItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
