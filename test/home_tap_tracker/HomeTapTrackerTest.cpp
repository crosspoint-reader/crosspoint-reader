#include <gtest/gtest.h>

#include "util/HomeTapTracker.h"

TEST(HomeTapTracker, ArmConsumesFrame) {
  HomeTapTracker tracker;
  EXPECT_FALSE(tracker.armed);
  tracker.arm(1000);
  EXPECT_TRUE(tracker.armed);
  EXPECT_EQ(tracker.armedAt, 1000);
}

TEST(HomeTapTracker, SecondTapWithinWindowIsDoubleClick) {
  HomeTapTracker tracker;
  tracker.arm(1000);
  EXPECT_EQ(tracker.update(true, 1100, 300), HomeTapTracker::Step::DoubleClick);
  EXPECT_FALSE(tracker.armed);
}

TEST(HomeTapTracker, ExpiryAtOrAfterWindow) {
  HomeTapTracker tracker;
  tracker.arm(1000);
  EXPECT_EQ(tracker.update(false, 1299, 300), HomeTapTracker::Step::None);
  EXPECT_TRUE(tracker.armed);
  EXPECT_EQ(tracker.update(false, 1300, 300), HomeTapTracker::Step::WindowExpired);
  EXPECT_FALSE(tracker.armed);
}

TEST(HomeTapTracker, DisarmResets) {
  HomeTapTracker tracker;
  tracker.arm(1000);
  tracker.disarm();
  EXPECT_FALSE(tracker.armed);
  EXPECT_EQ(tracker.update(true, 1100, 300), HomeTapTracker::Step::None);
}

TEST(HomeTapTracker, LongPressStyleForcedDisarm) {
  HomeTapTracker tracker;
  tracker.arm(1000);
  tracker.disarm();
  EXPECT_EQ(tracker.update(false, 2000, 300), HomeTapTracker::Step::None);
}

TEST(HomeTapTracker, RepeatedCycles) {
  HomeTapTracker tracker;
  for (int i = 0; i < 3; ++i) {
    tracker.arm(i * 1000);
    EXPECT_EQ(tracker.update(true, i * 1000 + 100, 300), HomeTapTracker::Step::DoubleClick);
    EXPECT_FALSE(tracker.armed);
  }
}

TEST(HomeTapTracker, WindowExpiresThenTapStartsNewCycle) {
  HomeTapTracker tracker;
  tracker.arm(0);
  EXPECT_EQ(tracker.update(false, 300, 300), HomeTapTracker::Step::WindowExpired);
  EXPECT_FALSE(tracker.armed);
  tracker.arm(400);
  EXPECT_EQ(tracker.update(true, 500, 300), HomeTapTracker::Step::DoubleClick);
  EXPECT_FALSE(tracker.armed);
}
