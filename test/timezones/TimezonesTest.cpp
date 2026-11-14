#include <gtest/gtest.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "HalClock.h"
#include "Timezones.h"

CrossPointSettings SETTINGS;
HalClock halClock;

namespace {

const char* nameAt(const int idx) { return timezones::table()[idx].name; }

TEST(IndexForOffset, DstZonesPreferDstRules) {
  // Berlin/Paris: +60 min standard, observes DST.
  int idx = timezones::indexForOffset(60, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Berlin / Paris / Madrid / Rome");
  // Athens: +120 min standard, observes DST.
  idx = timezones::indexForOffset(120, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Athens / Helsinki / Kyiv");
}

TEST(IndexForOffset, FixedZonesPreferNoDst) {
  int idx = timezones::indexForOffset(0, false);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "UTC");
  idx = timezones::indexForOffset(330, false);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "India / Colombo");
}

TEST(IndexForOffset, FallsBackToAnyEntryWithTheOffset) {
  // No named zone at +05:00 observes DST; still maps to the offset.
  const int idx = timezones::indexForOffset(300, true);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(timezones::table()[idx].stdOffsetQ, 20);
}

TEST(IndexForOffset, RejectsUnsupportedOffsets) {
  EXPECT_EQ(timezones::indexForOffset(7, false), -1);     // not a quarter hour
  EXPECT_EQ(timezones::indexForOffset(15 * 60, false), -1);  // beyond UTC+14
}

}  // namespace
