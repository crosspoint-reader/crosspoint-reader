#include "ReadingStatsActivity.h"

#include <I18n.h>
#include <TrustedTime.h>

#include "CrossPointSettings.h"
#include "StatsFormat.h"
#include "components/UITheme.h"
#include "stats/ReadingStatsStore.h"

namespace fui = freeink::ui;

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ReadingStats", renderer, mappedInput) {}

const char* ReadingStatsActivity::headerTitle() const { return tr(STR_READING_STATS); }

void ReadingStatsActivity::onEnter() {
  // The base onEnter schedules a paint; hold the lock until the rows are filled.
  RenderLock lock;
  UiListActivity::onEnter();
  const readingstats::Totals totals = readingstats::loadTotals();
  const int32_t today = readingstats::localDay(trustedtime::trustedNow());

  const StrId labels[ROW_COUNT] = {StrId::STR_STATS_TODAY,         StrId::STR_STATS_DAY_STREAK,
                                   StrId::STR_STATS_TIME_READ,     StrId::STR_STATS_SESSIONS,
                                   StrId::STR_STATS_PAGES,         StrId::STR_STATS_AVG_SESSION,
                                   StrId::STR_STATS_PAGES_PER_MIN, StrId::STR_STATS_BOOKS_FINISHED};
  rowValues[0] = statsformat::today(readingstats::secondsOnDay(totals, today), SETTINGS.dailyReadingGoalMinutes);
  rowValues[1] = statsformat::count(readingstats::currentStreak(totals, today));
  rowValues[2] = statsformat::duration(totals.seconds);
  rowValues[3] = statsformat::count(totals.sessions);
  rowValues[4] = statsformat::count(totals.pages);
  rowValues[5] = statsformat::averageSession(totals.sessionSeconds, totals.sessions);
  rowValues[6] = statsformat::pagesPerMinute(totals.pages, totals.seconds);
  rowValues[7] = statsformat::count(totals.finishedBooks);
  for (int i = 0; i < ROW_COUNT; i++) {
    rowItems[i].label = I18N.get(labels[i]);
    rowItems[i].value = rowValues[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

void ReadingStatsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}
