#include <gtest/gtest.h>

#include "ReadingCalendarModel.h"

namespace {
void setHistoryBit(ReadingCalendarSnapshot& snapshot, const size_t index) {
  snapshot.historyBits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
  ++snapshot.readingDays;
}

ReadingCalendarCell findDay(const ReadingCalendarModel& model, const uint8_t day) {
  for (size_t index = 0; index < 42; ++index) {
    const ReadingCalendarCell cell = model.cellAt(index);
    if (cell.inMonth && cell.date.day == day) return cell;
  }
  return {};
}
}  // namespace

TEST(ReadingCalendarModel, RequiresTrustedHistoryAndAValidClock) {
  ReadingCalendarSnapshot snapshot;
  snapshot.historyAvailable = true;
  ReadingCalendarModel noClock(snapshot);
  EXPECT_FALSE(noClock.isAvailable());
  EXPECT_FALSE(noClock.canMovePrevious());
  EXPECT_FALSE(noClock.canMoveNext());

  snapshot.today = {2026, 7, 24};
  snapshot.clockValid = true;
  snapshot.historyAvailable = false;
  ReadingCalendarModel noHistory(snapshot);
  EXPECT_FALSE(noHistory.isAvailable());
}

TEST(ReadingCalendarModel, MapsLeapMonthAndBinaryReadDays) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2024, 2, 29};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);   // 29 February
  setHistoryBit(snapshot, 28);  // 1 February

  ReadingCalendarModel model(snapshot);
  const ReadingCalendarCell first = findDay(model, 1);
  const ReadingCalendarCell last = findDay(model, 29);
  ASSERT_TRUE(first.inMonth);
  ASSERT_TRUE(last.inMonth);
  EXPECT_EQ(readingStatsDayOfWeekIndex(first.date), 3u);  // Thursday, Monday = 0
  EXPECT_TRUE(first.tracked);
  EXPECT_TRUE(first.read);
  EXPECT_TRUE(last.today);
  EXPECT_TRUE(last.read);
  EXPECT_FALSE(findDay(model, 30).inMonth);
}

TEST(ReadingCalendarModel, UsesAllSixRowsWhenTheMonthNeedsThem) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2021, 5, 31};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);

  ReadingCalendarModel model(snapshot);
  const ReadingCalendarCell last = model.cellAt(35);
  ASSERT_TRUE(last.inMonth);
  EXPECT_EQ(last.date.day, 31u);
  EXPECT_TRUE(last.read);
}

TEST(ReadingCalendarModel, DistinguishesTheOldestTrackedDayFromOlderUnknownDays) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.anchorDay = readingStatsDayIndex(snapshot.today);
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;
  setHistoryBit(snapshot, 0);
  setHistoryBit(snapshot, READING_HISTORY_DAYS - 1);

  ReadingStatsDate oldest;
  ASSERT_TRUE(readingStatsDateFromDayIndex(snapshot.anchorDay - (READING_HISTORY_DAYS - 1), oldest));
  ReadingCalendarModel model(snapshot);
  int moves = 0;
  while (model.movePrevious()) ++moves;
  EXPECT_GT(moves, 0);
  EXPECT_EQ(model.visibleMonth().year, oldest.year);
  EXPECT_EQ(model.visibleMonth().month, oldest.month);
  EXPECT_FALSE(model.canMovePrevious());

  const ReadingCalendarCell oldestCell = findDay(model, oldest.day);
  ASSERT_TRUE(oldestCell.inMonth);
  EXPECT_TRUE(oldestCell.tracked);
  EXPECT_TRUE(oldestCell.read);
  if (oldest.day > 1) {
    const ReadingCalendarCell before = findDay(model, static_cast<uint8_t>(oldest.day - 1));
    ASSERT_TRUE(before.inMonth);
    EXPECT_FALSE(before.tracked);
    EXPECT_FALSE(before.read);
  }
}

TEST(ReadingCalendarModel, EmptyHistoryShowsOnlyTheCurrentMonthAsKnown) {
  ReadingCalendarSnapshot snapshot;
  snapshot.today = {2026, 7, 24};
  snapshot.clockValid = true;
  snapshot.historyAvailable = true;

  ReadingCalendarModel model(snapshot);
  EXPECT_FALSE(model.canMovePrevious());
  EXPECT_FALSE(model.canMoveNext());
  const ReadingCalendarCell today = findDay(model, 24);
  const ReadingCalendarCell future = findDay(model, 25);
  ASSERT_TRUE(today.inMonth);
  ASSERT_TRUE(future.inMonth);
  EXPECT_TRUE(today.tracked);
  EXPECT_FALSE(today.read);
  EXPECT_FALSE(future.tracked);
}
