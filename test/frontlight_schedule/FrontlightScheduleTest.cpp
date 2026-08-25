#include <gtest/gtest.h>

#include "util/FrontlightSchedule.h"

namespace {

constexpr uint8_t slot(const uint8_t hour, const uint8_t quarter = 0) {
  return hour * FrontlightSchedule::SLOTS_PER_HOUR + quarter;
}

}  // namespace

TEST(FrontlightSchedule, DaytimeWindowUsesInclusiveStartAndExclusiveEnd) {
  EXPECT_FALSE(FrontlightSchedule::isActiveAtMinute(7 * 60 + 59, slot(8), slot(17)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(8 * 60, slot(8), slot(17)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(16 * 60 + 59, slot(8), slot(17)));
  EXPECT_FALSE(FrontlightSchedule::isActiveAtMinute(17 * 60, slot(8), slot(17)));
}

TEST(FrontlightSchedule, OvernightWindowWrapsAcrossMidnight) {
  EXPECT_FALSE(FrontlightSchedule::isActiveAtMinute(18 * 60 + 59, slot(19), slot(7)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(19 * 60, slot(19), slot(7)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(23 * 60 + 59, slot(19), slot(7)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(6 * 60 + 59, slot(19), slot(7)));
  EXPECT_FALSE(FrontlightSchedule::isActiveAtMinute(7 * 60, slot(19), slot(7)));
}

TEST(FrontlightSchedule, EqualTimesMeanAllDay) {
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(0, slot(12), slot(12)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(12 * 60, slot(12), slot(12)));
  EXPECT_TRUE(FrontlightSchedule::isActiveAtMinute(23 * 60 + 59, slot(12), slot(12)));
}

TEST(FrontlightSchedule, LocalMinuteAppliesQuarterHourOffsetAndWraps) {
  EXPECT_EQ(FrontlightSchedule::toLocalMinute(12, 0, 48), 12 * 60);
  EXPECT_EQ(FrontlightSchedule::toLocalMinute(20, 30, 48 + 23), 2 * 60 + 15);  // UTC+5:45
  EXPECT_EQ(FrontlightSchedule::toLocalMinute(3, 0, 48 - 16), 23 * 60);        // UTC-4:00
}
