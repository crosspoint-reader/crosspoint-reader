#include "ReadingCalendarModel.h"

#include <algorithm>
#include <utility>

namespace {
ReadingStatsDate firstOfMonth(const ReadingStatsDate& date) {
  return date.isValid() ? ReadingStatsDate{date.year, date.month, 1} : ReadingStatsDate{};
}

bool bitSet(const std::array<uint8_t, READING_HISTORY_BYTES>& bits, const uint32_t index) {
  return index < READING_HISTORY_DAYS &&
         (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}
}  // namespace

ReadingCalendarModel::ReadingCalendarModel(ReadingCalendarSnapshot snapshot) : snapshot_(std::move(snapshot)) {
  visibleMonth_ = firstOfMonth(snapshot_.today);
}

uint32_t ReadingCalendarModel::earliestTrackedDay() const {
  if (snapshot_.readingDays == 0) return readingStatsDayIndex(firstOfMonth(snapshot_.today));
  const uint32_t earliest = snapshot_.anchorDay >= READING_HISTORY_DAYS - 1
                                ? snapshot_.anchorDay - static_cast<uint32_t>(READING_HISTORY_DAYS - 1)
                                : 0;
  return std::min(earliest, readingStatsDayIndex(snapshot_.today));
}

bool ReadingCalendarModel::isReadDay(const uint32_t dayIndex) const {
  if (dayIndex > snapshot_.anchorDay) return false;
  const uint32_t delta = snapshot_.anchorDay - dayIndex;
  return delta < READING_HISTORY_DAYS && bitSet(snapshot_.historyBits, delta);
}

ReadingCalendarCell ReadingCalendarModel::cellAt(const size_t index) const {
  ReadingCalendarCell cell;
  if (!isAvailable() || !visibleMonth_.isValid() || index >= 42) return cell;
  const uint8_t firstColumn = readingStatsDayOfWeekIndex(visibleMonth_);
  const uint8_t monthDays = daysInMonth(visibleMonth_.year, visibleMonth_.month);
  if (index < firstColumn || index >= static_cast<size_t>(firstColumn) + monthDays) return cell;
  const uint8_t day = static_cast<uint8_t>(index - firstColumn + 1u);
  const uint32_t todayIndex = readingStatsDayIndex(snapshot_.today);
  const uint32_t earliest = earliestTrackedDay();
  cell.date = {visibleMonth_.year, visibleMonth_.month, day};
  const uint32_t dayIndex = readingStatsDayIndex(cell.date);
  cell.inMonth = true;
  cell.today = dayIndex == todayIndex;
  cell.tracked = dayIndex >= earliest && dayIndex <= todayIndex;
  cell.read = cell.tracked && isReadDay(dayIndex);
  return cell;
}

bool ReadingCalendarModel::canMovePrevious() const {
  if (!isAvailable() || !visibleMonth_.isValid()) return false;
  ReadingStatsDate earliest;
  if (!readingStatsDateFromDayIndex(earliestTrackedDay(), earliest)) return false;
  return compareReadingStatsDate(visibleMonth_, firstOfMonth(earliest)) > 0;
}

bool ReadingCalendarModel::canMoveNext() const {
  return isAvailable() && visibleMonth_.isValid() &&
         compareReadingStatsDate(visibleMonth_, firstOfMonth(snapshot_.today)) < 0;
}

bool ReadingCalendarModel::moveMonth(const int delta) {
  if (!visibleMonth_.isValid() || delta == 0) return false;
  int year = visibleMonth_.year;
  int month = static_cast<int>(visibleMonth_.month) + delta;
  while (month < 1) {
    month += 12;
    --year;
  }
  while (month > 12) {
    month -= 12;
    ++year;
  }
  if (year < 2000 || year > 2099) return false;
  const ReadingStatsDate candidate{static_cast<uint16_t>(year), static_cast<uint8_t>(month), 1};
  ReadingStatsDate earliest;
  if (!readingStatsDateFromDayIndex(earliestTrackedDay(), earliest)) return false;
  if (compareReadingStatsDate(candidate, firstOfMonth(earliest)) < 0 ||
      compareReadingStatsDate(candidate, firstOfMonth(snapshot_.today)) > 0) {
    return false;
  }
  visibleMonth_ = candidate;
  return true;
}

bool ReadingCalendarModel::movePrevious() {
  if (!canMovePrevious()) return false;
  return moveMonth(-1);
}

bool ReadingCalendarModel::moveNext() {
  if (!canMoveNext()) return false;
  return moveMonth(1);
}
