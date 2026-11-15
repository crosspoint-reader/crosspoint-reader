#include <gtest/gtest.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "HalClock.h"
#include "Timezones.h"

CrossPointSettings SETTINGS;
HalClock halClock;

namespace {

const char* nameAt(const int idx) { return timezones::table()[idx].name; }

TEST(IndexForZone, RegionSelectsDstRule) {
  int idx = timezones::indexForZone("Europe/Berlin", 60, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Berlin / Paris / Madrid / Rome");
  idx = timezones::indexForZone("Europe/Athens", 120, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Athens / Helsinki / Kyiv");
}

TEST(IndexForZone, DistinguishesSantiagoFromHalifax) {
  int idx = timezones::indexForZone("America/Santiago", -240, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Santiago");
  idx = timezones::indexForZone("America/Halifax", -240, true);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "Halifax");
}

TEST(IndexForZone, RegionOffsetMismatchRejected) {
  EXPECT_EQ(timezones::indexForZone("America/Santiago", 60, true), -1);
}

TEST(IndexForZone, UnknownRegionWithDstIsNotGuessed) {
  EXPECT_EQ(timezones::indexForZone("", -240, true), -1);
  EXPECT_EQ(timezones::indexForZone("Mars/Olympus", 60, true), -1);
}

TEST(IndexForZone, UnknownRegionWithoutDstUsesFixedRule) {
  int idx = timezones::indexForZone("", 0, false);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "UTC");
  idx = timezones::indexForZone(nullptr, 330, false);
  ASSERT_GE(idx, 0);
  EXPECT_STREQ(nameAt(idx), "India / Colombo");
}

TEST(IndexForZone, RejectsUnsupportedOffsets) {
  EXPECT_EQ(timezones::indexForZone("", 7, false), -1);
  EXPECT_EQ(timezones::indexForZone("", 15 * 60, false), -1);
}

}  // namespace
