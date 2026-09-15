#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <memory>

#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TimezonePickerActivity.h"
#include "components/UITheme.h"
#include "util/Timezones.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_TIMEZONE = 0,
  ITEM_DST,
  ITEM_FORMAT,
  ITEM_SHOW_ON_HOME,
  ITEM_SYNC,
};

const StrId menuNames[ClockSettingsActivity::ITEM_COUNT] = {
    StrId::STR_TIMEZONE,        StrId::STR_CLOCK_DST,      StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_IN_HEADER, StrId::STR_CLOCK_SYNC_NOW,
};

const StrId dstNames[CrossPointSettings::CLOCK_DST_MODE_COUNT] = {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON,
                                                                  StrId::STR_STATE_OFF};
}  // namespace

ClockSettingsActivity::ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ClockSettings", renderer, mappedInput) {}

void ClockSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* ClockSettingsActivity::headerTitle() const { return tr(STR_CLOCK); }

void ClockSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  switch (index) {
    case ITEM_TIMEZONE:
      startActivityForResult(std::make_unique<TimezonePickerActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_DST:
      SETTINGS.clockDst = (SETTINGS.clockDst + 1) % CrossPointSettings::CLOCK_DST_MODE_COUNT;
      timezones::applyToClock();
      break;
    case ITEM_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % 2;
      break;
    case ITEM_SHOW_ON_HOME:
      SETTINGS.clockShowInHeader = (SETTINGS.clockShowInHeader + 1) % 2;
      break;
    case ITEM_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
  }
  SETTINGS.saveToFile();
  requestUpdate();
}

void ClockSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowValues_[ITEM_TIMEZONE] = timezones::table()[timezones::activeIndex()].name;
  const uint8_t dst = SETTINGS.clockDst < CrossPointSettings::CLOCK_DST_MODE_COUNT ? SETTINGS.clockDst : uint8_t{0};
  rowValues_[ITEM_DST] = I18N.get(dstNames[dst]);
  rowValues_[ITEM_FORMAT] = SETTINGS.clockFormat == 1 ? tr(STR_CLOCK_FORMAT_12H) : tr(STR_CLOCK_FORMAT_24H);
  rowValues_[ITEM_SHOW_ON_HOME] = SETTINGS.clockShowInHeader ? tr(STR_SHOW) : tr(STR_HIDE);
  // The sync row's value is the current time itself: it confirms the sync,
  // previews format/zone changes, and reads "Not Set" until the first sync.
  char timeBuf[9];
  if (SETTINGS.clockHasBeenSynced && halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockFormat == 1)) {
    rowValues_[ITEM_SYNC] = timeBuf;
  } else {
    rowValues_[ITEM_SYNC] = tr(STR_NOT_SET);
  }
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
