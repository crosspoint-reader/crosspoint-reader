#include "BookStatsActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <utility>

#include "StatsFormat.h"
#include "components/UITheme.h"
#include "stats/ReadingStatsStore.h"

namespace fui = freeink::ui;

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                     const uint32_t liveSeconds, const uint32_t livePages)
    : UiListActivity("BookStats", renderer, mappedInput),
      bookPath(std::move(bookPath)),
      liveSeconds(liveSeconds),
      livePages(livePages) {}

const char* BookStatsActivity::headerTitle() const { return tr(STR_BOOK_STATS); }

void BookStatsActivity::onEnter() {
  // The base onEnter schedules a paint; hold the lock until the rows are filled.
  RenderLock lock;
  UiListActivity::onEnter();
  readingstats::Book book = readingstats::loadBook(bookPath);
  finished = book.finished;
  // Show the open session as it will be saved; the date only affects totals.
  readingstats::Totals unusedTotals;
  readingstats::applySession(unusedTotals, book, liveSeconds, livePages, 0);

  const StrId labels[FINISHED_ROW] = {StrId::STR_STATS_TIME_READ, StrId::STR_STATS_SESSIONS, StrId::STR_STATS_PAGES,
                                      StrId::STR_STATS_AVG_SESSION, StrId::STR_STATS_PAGES_PER_MIN};
  rowValues[0] = statsformat::duration(book.seconds);
  rowValues[1] = statsformat::count(book.sessions);
  rowValues[2] = statsformat::count(book.pages);
  rowValues[3] = statsformat::averageSession(book.sessionSeconds, book.sessions);
  rowValues[4] = statsformat::pagesPerMinute(book.pages, book.seconds);
  for (int i = 0; i < FINISHED_ROW; i++) {
    rowItems[i].label = I18N.get(labels[i]);
    rowItems[i].value = rowValues[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
  rowItems[FINISHED_ROW].label = tr(STR_STATS_FINISHED);
  rowItems[FINISHED_ROW].toggle = true;
  rowItems[FINISHED_ROW].toggleChecked = finished;
  rowItems[FINISHED_ROW].actionValue = FINISHED_ROW;
}

void BookStatsActivity::activateIndex(const int index) {
  if (index != FINISHED_ROW) return;
  if (!readingstats::setBookFinished(bookPath, !finished)) {
    LOG_ERR("STAT", "Could not save finished state");
    return;
  }
  {
    // buildScreen() reads the row on the render task.
    RenderLock lock;
    finished = !finished;
    rowItems[FINISHED_ROW].toggleChecked = finished;
  }
  requestUpdate();
}

void BookStatsActivity::buildScreen(UiScreen& screen) {
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
