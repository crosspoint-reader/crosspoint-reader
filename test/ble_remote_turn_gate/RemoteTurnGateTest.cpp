#include <gtest/gtest.h>

#include "activities/reader/RemoteTurnGate.h"

TEST(RemoteTurnGate, TurnsTheShownPage) { EXPECT_TRUE(remoteTurnAccepted(true, false, false)); }

TEST(RemoteTurnGate, LeavesAPageStillRenderingAlone) { EXPECT_FALSE(remoteTurnAccepted(true, true, false)); }

TEST(RemoteTurnGate, WaitsForTheFirstPage) { EXPECT_FALSE(remoteTurnAccepted(false, false, false)); }

// A toolbar, panel or end-of-book menu owns the device keys while it is shown; the book must not
// change behind it.
TEST(RemoteTurnGate, LeavesTheBookAloneUnderAToolbarOrMenu) {
  EXPECT_FALSE(remoteTurnAccepted(true, false, true));
  EXPECT_FALSE(remoteTurnAccepted(true, true, true));
}
