#include <gtest/gtest.h>

#include "src/util/QuickLockState.h"

TEST(QuickLockStateTest, LockTransitionPausesReading) {
  QuickLockState state;

  const auto transition = state.toggle(1000U);

  EXPECT_TRUE(state.isLocked());
  EXPECT_EQ(transition, QuickLockState::Transition::PauseReading);
}

TEST(QuickLockStateTest, UnlockTransitionResumesReading) {
  QuickLockState state;
  (void)state.toggle(1000U);

  const auto transition = state.toggle(2000U);

  EXPECT_FALSE(state.isLocked());
  EXPECT_EQ(transition, QuickLockState::Transition::ResumeReading);
}

TEST(QuickLockStateTest, SleepTimeoutStartsWhenLockEngages) {
  QuickLockState state;
  (void)state.toggle(1000U);

  EXPECT_FALSE(state.shouldSleep(5999U, 5000U));
  EXPECT_TRUE(state.shouldSleep(6000U, 5000U));
}

TEST(QuickLockStateTest, ZeroTimeoutMeansNeverSleep) {
  QuickLockState state;
  (void)state.toggle(1000U);

  EXPECT_FALSE(state.shouldSleep(100000U, 0U));
}

TEST(QuickLockStateTest, TimeoutArithmeticHandlesMillisWraparound) {
  QuickLockState state;
  (void)state.toggle(0xFFFFFF00U);

  EXPECT_FALSE(state.shouldSleep(0x00000050U, 0x200U));
  EXPECT_TRUE(state.shouldSleep(0x00000100U, 0x200U));
}
