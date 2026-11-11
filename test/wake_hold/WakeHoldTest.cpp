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
