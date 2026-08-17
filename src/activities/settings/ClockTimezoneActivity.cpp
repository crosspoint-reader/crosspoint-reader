#include "ClockTimezoneActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "ClockTimeZones.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

ClockTimezoneActivity::ClockTimezoneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ClockTimezone", renderer, mappedInput) {}

void ClockTimezoneActivity::onEnter() {
  UiListActivity::onEnter();
  if (SETTINGS.clockTimeZone >= CLOCK_TIME_ZONE_COUNT) SETTINGS.clockTimeZone = CLOCK_TZ_UTC;
  nav.selected = SETTINGS.clockTimeZone;
}

int ClockTimezoneActivity::listCount() const { return CLOCK_TIME_ZONE_COUNT; }

const char* ClockTimezoneActivity::headerTitle() const { return tr(STR_TIME_ZONE); }

void ClockTimezoneActivity::refreshRowWindow(const int start) {
  int clamped = start;
  if (clamped > listCount() - ROW_WINDOW) clamped = listCount() - ROW_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == windowStart) return;

  windowCount = listCount() - clamped < ROW_WINDOW ? listCount() - clamped : ROW_WINDOW;
  for (int i = 0; i < windowCount; ++i) {
    const int zoneIndex = clamped + i;
    fui::ListItem item;
    item.label = I18N.get(getClockTimeZone(static_cast<uint8_t>(zoneIndex)).label);
    item.value = zoneIndex == SETTINGS.clockTimeZone ? tr(STR_SELECTED) : nullptr;
    item.actionValue = static_cast<int16_t>(zoneIndex);
    rowItems[i] = item;
  }
  windowStart = clamped;
}

void ClockTimezoneActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (SETTINGS.clockTimeZone != index) {
    SETTINGS.clockTimeZone = static_cast<uint8_t>(index);
    SETTINGS.saveToFile();
  }
  finish();
}

void ClockTimezoneActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  refreshRowWindow(nav.top);
  props.items = rowItems;
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  screen.list(props);
}

