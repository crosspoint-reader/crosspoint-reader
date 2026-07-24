#include "ReadingStatsDateEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"

namespace {
constexpr std::array<StrId, ReadingStatsDateEditActivity::ROW_COUNT> ROW_LABELS = {
    StrId::STR_STATS_DATE_USE,   StrId::STR_STATS_DATE_DAY,   StrId::STR_STATS_DATE_MONTH,
    StrId::STR_STATS_DATE_YEAR,  StrId::STR_STATS_DATE_HOUR,  StrId::STR_STATS_DATE_MINUTE,
    StrId::STR_STATS_DATE_SAVE,
};
}

ReadingStatsDateEditActivity::ReadingStatsDateEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::string cachePath, BookReadingStats stats)
    : Activity("ReadingStatsDateEdit", renderer, mappedInput),
      cachePath(std::move(cachePath)),
      stats(std::move(stats)) {}

void ReadingStatsDateEditActivity::onEnter() {
  Activity::onEnter();
  if (!getCurrentLocalReadingStatsDateTime(fallbackDateTime)) {
    fallbackDateTime.date = {2000, 1, 1};
    fallbackDateTime.hour = 0;
    fallbackDateTime.minute = 0;
    fallbackDateTime.second = 0;
  }
  requestUpdate();
}

ReadingStatsDate& ReadingStatsDateEditActivity::currentDate() {
  return page == Page::Started ? stats.startDate : stats.finishedDate;
}

uint16_t& ReadingStatsDateEditActivity::currentMinuteOfDay() {
  return page == Page::Started ? stats.startMinuteOfDay : stats.finishedMinuteOfDay;
}

bool& ReadingStatsDateEditActivity::currentManualFlag() {
  return page == Page::Started ? stats.startDateManual : stats.finishedDateManual;
}

bool ReadingStatsDateEditActivity::currentTimestampIsSet() const {
  const ReadingStatsDate& date = page == Page::Started ? stats.startDate : stats.finishedDate;
  const uint16_t minute = page == Page::Started ? stats.startMinuteOfDay : stats.finishedMinuteOfDay;
  return date.isValid() && minute < 24u * 60u;
}

void ReadingStatsDateEditActivity::setCurrentTimestampEnabled(const bool enabled) {
  if (!enabled) {
    currentDate().clear();
    currentMinuteOfDay() = BookReadingStats::INVALID_MINUTE_OF_DAY;
    currentManualFlag() = false;
    return;
  }
  if (!currentTimestampIsSet()) {
    currentDate() = fallbackDateTime.date;
    currentMinuteOfDay() =
        static_cast<uint16_t>(fallbackDateTime.hour) * 60u + static_cast<uint16_t>(fallbackDateTime.minute);
  }
  currentManualFlag() = true;
}

void ReadingStatsDateEditActivity::openValueEditor(const int row) {
  if (row < 1 || row > 5) return;
  setCurrentTimestampEnabled(true);

  const ReadingStatsDate& date = currentDate();
  const uint16_t minute = currentMinuteOfDay();
  int initial = 0;
  int minimum = 0;
  int maximum = 0;
  int largeStep = 1;
  switch (row) {
    case 1:
      initial = date.day;
      minimum = 1;
      maximum = daysInMonth(date.year, date.month);
      largeStep = 5;
      break;
    case 2:
      initial = date.month;
      minimum = 1;
      maximum = 12;
      largeStep = 3;
      break;
    case 3:
      initial = date.year;
      minimum = 2000;
      maximum = 2099;
      largeStep = 10;
      break;
    case 4:
      initial = minute / 60u;
      minimum = 0;
      maximum = 23;
      largeStep = 6;
      break;
    case 5:
      initial = minute % 60u;
      minimum = 0;
      maximum = 59;
      largeStep = 10;
      break;
    default:
      return;
  }

  const StrId title = ROW_LABELS[static_cast<size_t>(row)];
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "ReadingStatsDateValue", title, initial,
                                                  minimum, maximum, 1, largeStep, StrId::STR_NONE_OPT, false, true),
      [this, row](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* interval = std::get_if<IntervalResult>(&result.data);
        if (interval) setEditedValue(row, interval->value);
      });
}

void ReadingStatsDateEditActivity::setEditedValue(const int row, const uint32_t value) {
  ReadingStatsDate& date = currentDate();
  uint16_t& minute = currentMinuteOfDay();
  switch (row) {
    case 1:
      date.day = static_cast<uint8_t>(value);
      break;
    case 2:
      date.month = static_cast<uint8_t>(value);
      date.day = std::min(date.day, daysInMonth(date.year, date.month));
      break;
    case 3:
      date.year = static_cast<uint16_t>(value);
      date.day = std::min(date.day, daysInMonth(date.year, date.month));
      break;
    case 4:
      minute = static_cast<uint16_t>(value * 60u + minute % 60u);
      break;
    case 5:
      minute = static_cast<uint16_t>(minute / 60u * 60u + value);
      break;
    default:
      return;
  }
  currentManualFlag() = true;
  requestUpdate();
}

bool ReadingStatsDateEditActivity::timestampsAreOrdered() const {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid() ||
      stats.startMinuteOfDay >= 24u * 60u || stats.finishedMinuteOfDay >= 24u * 60u) {
    return true;
  }
  const int dateOrder = compareReadingStatsDate(stats.startDate, stats.finishedDate);
  return dateOrder < 0 || (dateOrder == 0 && stats.startMinuteOfDay <= stats.finishedMinuteOfDay);
}

void ReadingStatsDateEditActivity::advanceOrSave() {
  if (page == Page::Started && stats.isCompleted) {
    page = Page::Finished;
    selectedIndex = 0;
    requestUpdate();
    return;
  }
  if (!timestampsAreOrdered() || !stats.save(cachePath)) {
    saveFailed = true;
    requestUpdate();
    return;
  }
  setResult(ActivityResult{});
  finish();
}

std::string ReadingStatsDateEditActivity::rowValue(const int row) const {
  if (row == 0) return I18N.get(currentTimestampIsSet() ? StrId::STR_YES : StrId::STR_NO);
  if (row == ROW_COUNT - 1) return {};
  if (!currentTimestampIsSet()) return tr(STR_NOT_SET);

  const ReadingStatsDate& date = page == Page::Started ? stats.startDate : stats.finishedDate;
  const uint16_t minute = page == Page::Started ? stats.startMinuteOfDay : stats.finishedMinuteOfDay;
  char value[8];
  switch (row) {
    case 1:
      snprintf(value, sizeof(value), "%02u", static_cast<unsigned>(date.day));
      break;
    case 2:
      snprintf(value, sizeof(value), "%02u", static_cast<unsigned>(date.month));
      break;
    case 3:
      snprintf(value, sizeof(value), "%04u", static_cast<unsigned>(date.year));
      break;
    case 4:
      snprintf(value, sizeof(value), "%02u", static_cast<unsigned>(minute / 60u));
      break;
    case 5:
      snprintf(value, sizeof(value), "%02u", static_cast<unsigned>(minute % 60u));
      break;
    default:
      value[0] = '\0';
      break;
  }
  return value;
}

void ReadingStatsDateEditActivity::loop() {
  if (saveFailed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      saveFailed = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (page == Page::Finished) {
      page = Page::Started;
      selectedIndex = 0;
      requestUpdate();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == 0) {
      setCurrentTimestampEnabled(!currentTimestampIsSet());
      requestUpdate();
    } else if (selectedIndex == ROW_COUNT - 1) {
      advanceOrSave();
    } else {
      openValueEditor(selectedIndex);
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });
}

void ReadingStatsDateEditActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = screen.y + metrics.topPadding;
  const int subHeaderTop = headerTop + metrics.headerHeight;
  const int contentTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height;

  GUI.drawHeader(renderer, Rect{screen.x, headerTop, screen.width, metrics.headerHeight}, tr(STR_STATS_EDIT_DATES));
  GUI.drawSubHeader(renderer, Rect{screen.x, subHeaderTop, screen.width, metrics.tabBarHeight},
                    I18N.get(page == Page::Started ? StrId::STR_STATS_STARTED_DATE
                                                  : StrId::STR_STATS_FINISHED_DATE),
                    stats.isCompleted ? (page == Page::Started ? "1/2" : "2/2") : "1/1");
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)}, ROW_COUNT,
      selectedIndex,
      [this](const int index) {
        if (index == ROW_COUNT - 1 && page == Page::Started && stats.isCompleted) {
          return std::string(tr(STR_STATS_DATE_NEXT));
        }
        return std::string(I18N.get(ROW_LABELS[static_cast<size_t>(index)]));
      },
      nullptr, nullptr, [this](const int index) { return rowValue(index); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (saveFailed) GUI.drawPopup(renderer, tr(STR_STATS_DATE_SAVE_FAILED));
  renderer.displayBuffer();
}
