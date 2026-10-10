#include <gtest/gtest.h>

#include "ReadingSpeedTracker.h"

TEST(ReadingSpeedTrackerTest, DefaultBaselineAverage) {
  ReadingSpeedTracker tracker;
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), ReadingSpeedTracker::DEFAULT_SEC_PER_PAGE);

  // 10 pages left at 45 sec/page = 450s = 7.5m -> rounded to 8m
  EXPECT_EQ(tracker.getMinutesLeftInChapter(1, 11), 8);

  // If already at or past the end of the chapter
  EXPECT_EQ(tracker.getMinutesLeftInChapter(10, 10), 0);
  EXPECT_EQ(tracker.getMinutesLeftInChapter(11, 10), 0);
}

TEST(ReadingSpeedTrackerTest, IgnoresRapidSkimAndLongIdleOutliers) {
  ReadingSpeedTracker tracker;

  // Simulate entering page at t = 1000ms
  tracker.onPageEntered(1000);

  // Rapid skim: turned after 1500ms (delta = 1500ms < MIN_PAGE_TIME_MS of 2000ms)
  tracker.onPageTurned(2500);
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), ReadingSpeedTracker::DEFAULT_SEC_PER_PAGE);

  // Long idle pause: next page turned after 301,000ms (delta > MAX_PAGE_TIME_MS of 300,000ms)
  tracker.onPageEntered(10000);
  tracker.onPageTurned(311000);
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), ReadingSpeedTracker::DEFAULT_SEC_PER_PAGE);
}

TEST(ReadingSpeedTrackerTest, ComputesRollingAverageWithCircularBuffer) {
  ReadingSpeedTracker tracker;

  // Turn 1: 30 seconds (30,000 ms)
  tracker.onPageEntered(0);
  tracker.onPageTurned(30000);
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), 30u);

  // Turn 2: 60 seconds (60,000 ms) -> (30 + 60) / 2 = 45s
  tracker.onPageEntered(30000);
  tracker.onPageTurned(90000);
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), 45u);

  // Add 6 more samples of 60 seconds (total 8 samples: one 30s, seven 60s -> sum = 450, 450/8 = 56s)
  uint32_t t = 90000;
  for (int i = 0; i < 6; ++i) {
    tracker.onPageEntered(t);
    t += 60000;
    tracker.onPageTurned(t);
  }
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), 56u);

  // 9th sample of 60 seconds replaces the oldest sample (30s) -> all eight samples are now 60s
  tracker.onPageEntered(t);
  t += 60000;
  tracker.onPageTurned(t);
  EXPECT_EQ(tracker.getAverageSecondsPerPage(), 60u);
}

TEST(ReadingSpeedTrackerTest, EstimatesBookTimeLeft) {
  ReadingSpeedTracker tracker;

  // Completed book
  EXPECT_EQ(tracker.getMinutesLeftInBook(100.0f, 10, 10), 0);
  EXPECT_EQ(tracker.getMinutesLeftInBook(99.6f, 10, 10), 0);

  // Early in book (< 1% progress)
  EXPECT_GT(tracker.getMinutesLeftInBook(0.5f, 1, 10), 0);

  // Mid-book: 50% progress, page 10 of chapter with 20 pages
  // At 50% progress, remaining fraction is 0.5
  int minLeft = tracker.getMinutesLeftInBook(50.0f, 10, 20);
  EXPECT_GT(minLeft, 0);
}

TEST(ReadingSpeedTrackerTest, FormatsTimeLeftCorrectly) {
  char buf[32];

  // 0 or negative
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 0);
  EXPECT_STREQ(buf, "");
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), -5);
  EXPECT_STREQ(buf, "");

  // Under an hour
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 14);
  EXPECT_STREQ(buf, "14m");

  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 59);
  EXPECT_STREQ(buf, "59m");

  // Exact hours
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 60);
  EXPECT_STREQ(buf, "1h");

  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 120);
  EXPECT_STREQ(buf, "2h");

  // Hours and minutes
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 75);
  EXPECT_STREQ(buf, "1h 15m");

  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 135);
  EXPECT_STREQ(buf, "2h 15m");

  // Custom language units
  ReadingSpeedTracker::formatTimeLeft(buf, sizeof(buf), 75, "min", "Std");
  EXPECT_STREQ(buf, "1Std 15min");
}
