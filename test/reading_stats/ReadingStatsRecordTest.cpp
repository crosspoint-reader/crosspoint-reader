#include <gtest/gtest.h>

#include "ReadingStatsRecord.h"

namespace {

using readingstats::Book;
using readingstats::Totals;

TEST(ReadingStatsRecord, CivilDayMatchesKnownDates) {
  EXPECT_EQ(readingstats::civilDay(1970, 1, 1), 0);
  EXPECT_EQ(readingstats::civilDay(2000, 3, 1), 11017);
  EXPECT_EQ(readingstats::civilDay(2026, 11, 24), 20781);
  EXPECT_EQ(readingstats::civilDay(2024, 3, 1) - readingstats::civilDay(2024, 2, 28), 2);  // leap year
}

TEST(ReadingStatsRecord, ShortSessionAddsTimeAndPagesButNotASession) {
  Totals totals;
  Book book;
  readingstats::applySession(totals, book, 59, 3, 100);
  EXPECT_EQ(totals.seconds, 59u);
  EXPECT_EQ(totals.pages, 3u);
  EXPECT_EQ(totals.sessions, 0u);
  EXPECT_EQ(book.seconds, 59u);
  EXPECT_EQ(book.sessions, 0u);

  readingstats::applySession(totals, book, 60, 2, 100);
  EXPECT_EQ(totals.sessions, 1u);
  EXPECT_EQ(book.sessions, 1u);
  EXPECT_EQ(book.pages, 5u);
  // Only the counted session enters the average.
  EXPECT_EQ(totals.sessionSeconds, 60u);
  EXPECT_EQ(book.sessionSeconds, 60u);
  EXPECT_EQ(totals.seconds, 119u);
}

TEST(ReadingStatsRecord, StreakGrowsOnConsecutiveDaysAndRestartsAfterAGap) {
  Totals totals;
  Book book;
  readingstats::applySession(totals, book, 120, 1, 100);
  readingstats::applySession(totals, book, 120, 1, 100);  // same day
  EXPECT_EQ(totals.streakDays, 1);
  readingstats::applySession(totals, book, 120, 1, 101);
  readingstats::applySession(totals, book, 120, 1, 102);
  EXPECT_EQ(totals.streakDays, 3);
  EXPECT_EQ(totals.lastReadDay, 102);
  readingstats::applySession(totals, book, 120, 1, 105);
  EXPECT_EQ(totals.streakDays, 1);
}

TEST(ReadingStatsRecord, StreakIgnoresUnknownAndEarlierDates) {
  Totals totals;
  Book book;
  readingstats::applySession(totals, book, 120, 1, 200);
  readingstats::applySession(totals, book, 120, 1, 0);    // no trusted date
  readingstats::applySession(totals, book, 120, 1, 150);  // clock moved back
  EXPECT_EQ(totals.streakDays, 1);
  EXPECT_EQ(totals.lastReadDay, 200);
  EXPECT_EQ(totals.seconds, 360u);
}

TEST(ReadingStatsRecord, TodayCountsOnlyTheLatestReadingDay) {
  Totals totals;
  Book book;
  readingstats::applySession(totals, book, 300, 1, 400);
  readingstats::applySession(totals, book, 120, 1, 400);
  EXPECT_EQ(readingstats::secondsOnDay(totals, 400), 420u);
  EXPECT_EQ(readingstats::secondsOnDay(totals, 401), 0u);
  readingstats::applySession(totals, book, 60, 1, 401);
  EXPECT_EQ(readingstats::secondsOnDay(totals, 401), 60u);
  readingstats::applySession(totals, book, 600, 1, 400);  // clock moved back
  EXPECT_EQ(readingstats::secondsOnDay(totals, 401), 60u);
  EXPECT_EQ(readingstats::secondsOnDay(totals, 0), 0u);
}

TEST(ReadingStatsRecord, CurrentStreakSurvivesUntilAFullDayIsMissed) {
  Totals totals;
  totals.lastReadDay = 300;
  totals.streakDays = 4;
  EXPECT_EQ(readingstats::currentStreak(totals, 300), 4);
  EXPECT_EQ(readingstats::currentStreak(totals, 301), 4);
  EXPECT_EQ(readingstats::currentStreak(totals, 302), 0);
  EXPECT_EQ(readingstats::currentStreak(totals, 0), 0);
  EXPECT_EQ(readingstats::currentStreak(Totals{}, 300), 0);
}

TEST(ReadingStatsRecord, FinishedStateKeepsTotalInStep) {
  Totals totals;
  Book book;
  EXPECT_TRUE(readingstats::setFinished(totals, book, true));
  EXPECT_FALSE(readingstats::setFinished(totals, book, true));
  EXPECT_EQ(totals.finishedBooks, 1u);
  EXPECT_TRUE(readingstats::setFinished(totals, book, false));
  EXPECT_EQ(totals.finishedBooks, 0u);
  EXPECT_FALSE(book.finished);
  Book other;
  EXPECT_FALSE(readingstats::setFinished(totals, other, false));
  EXPECT_EQ(totals.finishedBooks, 0u);
}

TEST(ReadingStatsRecord, CountersSaturateInsteadOfWrapping) {
  Totals totals;
  totals.seconds = UINT32_MAX - 1;
  Book book;
  readingstats::applySession(totals, book, 100, 0, 0);
  EXPECT_EQ(totals.seconds, UINT32_MAX);
}

TEST(ReadingStatsRecord, TotalsRoundTripAndRejectCorruption) {
  Totals totals;
  totals.seconds = 123456;
  totals.sessions = 42;
  totals.sessionSeconds = 120000;
  totals.pages = 9876;
  totals.finishedBooks = 7;
  totals.lastReadDay = 20781;
  totals.streakDays = 12;
  totals.lastDaySeconds = 900;
  uint8_t data[readingstats::TOTALS_BYTES];
  readingstats::encode(totals, data);

  Totals decoded;
  ASSERT_TRUE(readingstats::decode(data, sizeof(data), decoded));
  EXPECT_EQ(decoded.seconds, totals.seconds);
  EXPECT_EQ(decoded.sessions, totals.sessions);
  EXPECT_EQ(decoded.sessionSeconds, totals.sessionSeconds);
  EXPECT_EQ(decoded.pages, totals.pages);
  EXPECT_EQ(decoded.finishedBooks, totals.finishedBooks);
  EXPECT_EQ(decoded.lastReadDay, totals.lastReadDay);
  EXPECT_EQ(decoded.streakDays, totals.streakDays);
  EXPECT_EQ(decoded.lastDaySeconds, totals.lastDaySeconds);

  data[8] ^= 0x01;
  Totals untouched;
  untouched.seconds = 5;
  EXPECT_FALSE(readingstats::decode(data, sizeof(data), untouched));
  EXPECT_EQ(untouched.seconds, 5u);
  EXPECT_FALSE(readingstats::decode(data, sizeof(data) - 1, untouched));
}

TEST(ReadingStatsRecord, BookRoundTripAndRejectsTotalsRecord) {
  Book book;
  book.seconds = 3600;
  book.sessions = 3;
  book.sessionSeconds = 3500;
  book.pages = 150;
  book.finished = true;
  uint8_t data[readingstats::BOOK_BYTES];
  readingstats::encode(book, data);

  Book decoded;
  ASSERT_TRUE(readingstats::decode(data, sizeof(data), decoded));
  EXPECT_EQ(decoded.seconds, 3600u);
  EXPECT_EQ(decoded.sessions, 3u);
  EXPECT_EQ(decoded.sessionSeconds, 3500u);
  EXPECT_EQ(decoded.pages, 150u);
  EXPECT_TRUE(decoded.finished);

  uint8_t totalsData[readingstats::TOTALS_BYTES];
  readingstats::encode(Totals{}, totalsData);
  EXPECT_FALSE(readingstats::decode(totalsData, sizeof(totalsData), decoded));
}

}  // namespace
