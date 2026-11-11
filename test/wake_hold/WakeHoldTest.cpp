#include <gtest/gtest.h>

#include "WakeHold.h"

TEST(WakeHold, AButtonWakeNeedsAHoldWhenTheSettingIsOn) { EXPECT_TRUE(wakeNeedsLongPress(true, true, false)); }

// Off is the default: waking is left exactly as it was.
TEST(WakeHold, TheSettingOffChangesNothing) {
  EXPECT_FALSE(wakeNeedsLongPress(false, true, false));
  EXPECT_FALSE(wakeNeedsLongPress(false, false, false));
  EXPECT_FALSE(wakeNeedsLongPress(false, true, true));
}

// USB power, a timer and a boot after flashing are not presses that could be accidental.
TEST(WakeHold, OtherWakeSourcesAreNotHeldUp) { EXPECT_FALSE(wakeNeedsLongPress(true, false, false)); }

// A silent reboot restarts with the button released; asking for a hold would put the device to sleep.
TEST(WakeHold, ASilentRebootIsNotHeldUp) {
  EXPECT_FALSE(wakeNeedsLongPress(true, true, true));
  EXPECT_FALSE(wakeNeedsLongPress(true, false, true));
}

TEST(WakeHoldWatch, AButtonKeptDownIsHeld) {
  WakeHoldWatch watch;
  for (unsigned long ms = 300; ms <= kWakeHoldMs + 100; ms += 5) watch.sample(true, ms);
  EXPECT_TRUE(watch.held());
}

TEST(WakeHoldWatch, LettingGoBeforeTheDeadlineBreaksTheHold) {
  WakeHoldWatch watch;
  watch.sample(true, 300);
  watch.sample(false, kWakeHoldMs - 1);
  EXPECT_FALSE(watch.held());
}

// The setting is read late: a hold that lasted the whole time still counts after the button is up.
TEST(WakeHoldWatch, LettingGoAfterTheDeadlineStillCounts) {
  WakeHoldWatch watch;
  watch.sample(true, kWakeHoldMs - 5);
  watch.sample(false, kWakeHoldMs);
  watch.sample(false, kWakeHoldMs + 400);
  EXPECT_TRUE(watch.held());
}

// Two quick presses are not one hold.
TEST(WakeHoldWatch, PressingAgainDoesNotMendABrokenHold) {
  WakeHoldWatch watch;
  watch.sample(true, 300);
  watch.sample(false, 350);
  watch.sample(true, 400);
  watch.sample(true, kWakeHoldMs + 50);
  EXPECT_FALSE(watch.held());
}
