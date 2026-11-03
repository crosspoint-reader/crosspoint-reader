#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "util/NightSchedule.h"

using nightschedule::latestBoundary;

namespace {

constexpr uint8_t slot(int hour, int minute = 0) { return static_cast<uint8_t>(hour * 2 + minute / 30); }

int64_t local(int year, int month, int day, int hour, int minute = 0) {
  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&t));
}

class NightScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
  }
};

}  // namespace

TEST_F(NightScheduleTest, EveningIsInsideTheNightWindow) {
  const auto b = latestBoundary(local(2026, 10, 27, 23), slot(22), slot(7));
  EXPECT_EQ(b.at, local(2026, 10, 27, 22));
  EXPECT_TRUE(b.nightOn);
}

TEST_F(NightScheduleTest, WindowPastMidnightReachesBackToYesterday) {
  const auto b = latestBoundary(local(2026, 10, 28, 3), slot(22), slot(7));
  EXPECT_EQ(b.at, local(2026, 10, 27, 22));
  EXPECT_TRUE(b.nightOn);
}

TEST_F(NightScheduleTest, MorningIsAfterTheOffTime) {
  const auto b = latestBoundary(local(2026, 10, 28, 8), slot(22), slot(7));
  EXPECT_EQ(b.at, local(2026, 10, 28, 7));
  EXPECT_FALSE(b.nightOn);
}

TEST_F(NightScheduleTest, BoundaryCountsAtItsExactMinute) {
  const auto b = latestBoundary(local(2026, 10, 27, 22), slot(22), slot(7));
  EXPECT_EQ(b.at, local(2026, 10, 27, 22));
  EXPECT_TRUE(b.nightOn);
}

TEST_F(NightScheduleTest, HalfHourSlots) {
  const auto b = latestBoundary(local(2026, 10, 27, 21, 45), slot(21, 30), slot(6, 30));
  EXPECT_EQ(b.at, local(2026, 10, 27, 21, 30));
  EXPECT_TRUE(b.nightOn);
}

TEST_F(NightScheduleTest, WindowInsideOneDay) {
  const auto b = latestBoundary(local(2026, 10, 27, 12), slot(8), slot(20));
  EXPECT_EQ(b.at, local(2026, 10, 27, 8));
  EXPECT_TRUE(b.nightOn);
  const auto late = latestBoundary(local(2026, 10, 27, 23), slot(8), slot(20));
  EXPECT_EQ(late.at, local(2026, 10, 27, 20));
  EXPECT_FALSE(late.nightOn);
}

// The caller keeps a manual change while the boundary it last applied is
// still the latest one, so the whole window must report the same boundary.
TEST_F(NightScheduleTest, SameBoundaryAcrossTheWholeWindow) {
  const auto evening = latestBoundary(local(2026, 10, 27, 22, 30), slot(22), slot(7));
  const auto lateNight = latestBoundary(local(2026, 10, 28, 6, 59), slot(22), slot(7));
  EXPECT_EQ(evening.at, lateNight.at);
}

TEST_F(NightScheduleTest, SameTimeNextDayIsANewBoundary) {
  const auto tonight = latestBoundary(local(2026, 10, 27, 23), slot(22), slot(7));
  const auto threeDaysOn = latestBoundary(local(2026, 10, 30, 23), slot(22), slot(7));
  EXPECT_NE(tonight.at, threeDaysOn.at);
  EXPECT_EQ(threeDaysOn.at, local(2026, 10, 30, 22));
}

// 2026-03-08 02:00 EST springs forward to 03:00 EDT: the off-time stays at
// 07:00 on the wall clock, eight real hours after 22:00 the night before.
TEST_F(NightScheduleTest, FollowsTheWallClockAcrossDaylightSaving) {
  const auto b = latestBoundary(local(2026, 3, 8, 7, 30), slot(22), slot(7));
  EXPECT_EQ(b.at, local(2026, 3, 8, 7));
  EXPECT_FALSE(b.nightOn);
  EXPECT_EQ(b.at - local(2026, 3, 7, 22), 8 * 3600);
}

// 02:30 doesn't exist on 2026-03-08. mktime would move it to 03:30, after the
// 03:00 off-time, and leave night mode on for a day; it is skipped instead.
TEST_F(NightScheduleTest, SkipsAnOnTimeInTheSpringForwardGap) {
  const auto b = latestBoundary(local(2026, 3, 8, 4), slot(2, 30), slot(3));
  EXPECT_EQ(b.at, local(2026, 3, 8, 3));
  EXPECT_FALSE(b.nightOn);
}

TEST_F(NightScheduleTest, EqualTimesMeanNoSchedule) {
  const auto b = latestBoundary(local(2026, 10, 27, 23), slot(22), slot(22));
  EXPECT_EQ(b.at, 0);
}

TEST_F(NightScheduleTest, OutOfRangeSlotMeansNoSchedule) {
  EXPECT_EQ(latestBoundary(local(2026, 10, 27, 23), 48, slot(7)).at, 0);
  EXPECT_EQ(latestBoundary(local(2026, 10, 27, 23), slot(22), 200).at, 0);
}

TEST(NightScheduleFormat, TwentyFourHour) {
  char buf[12];
  nightschedule::formatSlot(0, false, buf, sizeof(buf));
  EXPECT_STREQ(buf, "00:00");
  nightschedule::formatSlot(35, false, buf, sizeof(buf));
  EXPECT_STREQ(buf, "17:30");
}

TEST(NightScheduleFormat, TwelveHour) {
  char buf[12];
  nightschedule::formatSlot(0, true, buf, sizeof(buf));
  EXPECT_STREQ(buf, "12:00 AM");
  EXPECT_EQ(strlen(buf) + 1, nightschedule::SLOT_LABEL_SIZE);
  nightschedule::formatSlot(24, true, buf, sizeof(buf));
  EXPECT_STREQ(buf, "12:00 PM");
  nightschedule::formatSlot(35, true, buf, sizeof(buf));
  EXPECT_STREQ(buf, "5:30 PM");
}
