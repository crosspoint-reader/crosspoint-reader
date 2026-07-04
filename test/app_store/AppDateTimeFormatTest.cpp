#include <gtest/gtest.h>

#include "lib/AppStore/AppDateTimeFormat.h"

TEST(AppDateTimeFormatTest, FormatsIso8601For24HourDisplay) {
  const std::string display = AppDateTimeFormat::formatIso8601ForDisplay("2026-07-03T19:30:00Z", 48, false);
  EXPECT_EQ(display, "Jul 3, 2026 19:30");
}

TEST(AppDateTimeFormatTest, AppliesUtcOffset) {
  const std::string display = AppDateTimeFormat::formatIso8601ForDisplay("2026-07-03T19:30:00Z", 52, false);
  EXPECT_EQ(display, "Jul 3, 2026 20:30");
}

TEST(AppDateTimeFormatTest, FormatsIso8601For12HourDisplay) {
  const std::string display = AppDateTimeFormat::formatIso8601ForDisplay("2026-07-03T19:30:00Z", 48, true);
  EXPECT_EQ(display, "Jul 3, 2026 7:30 PM");
}

TEST(AppDateTimeFormatTest, RejectsEmptyOrInvalidIso) {
  EXPECT_TRUE(AppDateTimeFormat::formatIso8601ForDisplay("", 48, false).empty());
  EXPECT_TRUE(AppDateTimeFormat::formatIso8601ForDisplay("not-a-date", 48, false).empty());
}

TEST(AppDateTimeFormatTest, ComparesInstalledAtDescending) {
  EXPECT_TRUE(AppDateTimeFormat::isNewerInstalledAt("2026-07-04T10:00:00Z", "2026-07-03T10:00:00Z"));
  EXPECT_FALSE(AppDateTimeFormat::isNewerInstalledAt("2026-07-03T10:00:00Z", "2026-07-04T10:00:00Z"));
  EXPECT_FALSE(AppDateTimeFormat::isNewerInstalledAt("", "2026-07-03T10:00:00Z"));
  EXPECT_TRUE(AppDateTimeFormat::isNewerInstalledAt("2026-07-03T10:00:00Z", ""));
}
