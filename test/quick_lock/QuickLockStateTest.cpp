#include <gtest/gtest.h>

#include "src/util/QuickLockState.h"

TEST(QuickLockStateTest, LockAndUnlock) {
  QuickLockState state;

  state.lock(1000U);
  EXPECT_TRUE(state.isLocked());

  state.unlock();
  EXPECT_FALSE(state.isLocked());
}

TEST(QuickLockStateTest, SleepTimeoutStartsWhenLockEngages) {
  QuickLockState state;
  state.lock(1000U);

  EXPECT_FALSE(state.shouldSleep(5999U, 5000U));
  EXPECT_TRUE(state.shouldSleep(6000U, 5000U));
}

TEST(QuickLockStateTest, ActivityRestartsSleepTimeout) {
  QuickLockState state;
  state.lock(1000U);

  state.noteActivity(4000U);

  EXPECT_FALSE(state.shouldSleep(8999U, 5000U));
  EXPECT_TRUE(state.shouldSleep(9000U, 5000U));
}

TEST(QuickLockStateTest, UnlockedNeverSleeps) {
  QuickLockState state;
  state.lock(0U);
  state.unlock();

  EXPECT_FALSE(state.shouldSleep(100000U, 5000U));
}

TEST(QuickLockStateTest, ZeroTimeoutMeansNeverSleep) {
  QuickLockState state;
  state.lock(1000U);

  EXPECT_FALSE(state.shouldSleep(100000U, 0U));
}

TEST(QuickLockStateTest, TimeoutArithmeticHandlesMillisWraparound) {
  QuickLockState state;
  state.lock(0xFFFFFF00U);

  EXPECT_FALSE(state.shouldSleep(0x00000050U, 0x200U));
  EXPECT_TRUE(state.shouldSleep(0x00000100U, 0x200U));
}
