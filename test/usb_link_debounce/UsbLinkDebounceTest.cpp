// UsbLinkDebounce (lib/hal/UsbLinkDebounce.h): which raw USB readings count as a plug or an unplug.
// The loop samples once per pass, every ~10 ms; these tests replay such passes. The dropouts are the
// ones the X3 logged while a host stayed attached: the link read absent for 12-46 ms, each one then
// repainting the page on screen twice.

#include <gtest/gtest.h>

#include "hal/UsbLinkDebounce.h"

namespace {

constexpr unsigned long kPassMs = 10;

struct Loop {
  UsbLinkDebounce link;
  unsigned long now = 1000;
  int edges = 0;

  // `ms` of loop passes that all read `raw`.
  void run(const bool raw, const unsigned long ms) {
    for (unsigned long t = 0; t < ms; t += kPassMs) {
      if (link.update(raw, now)) edges++;
      now += kPassMs;
    }
  }
};

TEST(UsbLinkDebounce, AnAttachCountsAtOnce) {
  Loop loop;
  loop.run(false, 100);
  EXPECT_EQ(0, loop.edges);
  EXPECT_TRUE(loop.link.update(true, loop.now));
  EXPECT_TRUE(loop.link.connected());
}

TEST(UsbLinkDebounce, AHostPresentAtBootIsOneEdge) {
  // updateUsbState()'s first call: main.cpp opens the serial log on that edge.
  UsbLinkDebounce link;
  EXPECT_TRUE(link.update(true, 0));
  EXPECT_FALSE(link.update(true, 10));
}

TEST(UsbLinkDebounce, TheDropoutsTheX3LoggedAreNoEdges) {
  Loop loop;
  loop.run(true, 100);
  ASSERT_EQ(1, loop.edges);
  for (const unsigned long dropout : {12ul, 15ul, 23ul, 31ul, 34ul, 46ul}) {
    loop.run(false, dropout);
    loop.run(true, 300);
  }
  EXPECT_EQ(1, loop.edges);
  EXPECT_TRUE(loop.link.connected());
}

TEST(UsbLinkDebounce, DropoutsInARowAreNoEdgesEither) {
  // Three inside one page turn: absent 33 ms, back 177 ms, absent 37 ms, back 23 ms, absent 32 ms.
  Loop loop;
  loop.run(true, 100);
  loop.run(false, 33);
  loop.run(true, 177);
  loop.run(false, 37);
  loop.run(true, 23);
  loop.run(false, 32);
  loop.run(true, 100);
  EXPECT_EQ(1, loop.edges);
}

TEST(UsbLinkDebounce, AnUnplugCountsOnceItHasHeld) {
  Loop loop;
  loop.run(true, 100);
  loop.run(false, UsbLinkDebounce::kDetachHoldMs);  // the last of these passes is 490 ms in
  EXPECT_TRUE(loop.link.connected());
  EXPECT_EQ(1, loop.edges);
  loop.run(false, kPassMs);  // 500 ms in
  EXPECT_FALSE(loop.link.connected());
  EXPECT_EQ(2, loop.edges);
  loop.run(false, 5000);
  EXPECT_EQ(2, loop.edges);  // one edge, not one per pass
}

TEST(UsbLinkDebounce, TheHoldStartsAgainAfterEveryReturn) {
  // Two dropouts just short of the hold, with one good pass between, are two dropouts -- not one
  // long absence.
  Loop loop;
  loop.run(true, 100);
  loop.run(false, UsbLinkDebounce::kDetachHoldMs);
  loop.run(true, kPassMs);
  loop.run(false, UsbLinkDebounce::kDetachHoldMs);
  EXPECT_TRUE(loop.link.connected());
  EXPECT_EQ(1, loop.edges);
}

TEST(UsbLinkDebounce, AReplugAfterAnUnplugIsAnEdgeAtOnce) {
  Loop loop;
  loop.run(true, 100);
  loop.run(false, 2000);
  ASSERT_EQ(2, loop.edges);
  ASSERT_FALSE(loop.link.connected());
  loop.run(true, kPassMs);
  EXPECT_EQ(3, loop.edges);
  EXPECT_TRUE(loop.link.connected());
}

TEST(UsbLinkDebounce, TheHoldSurvivesMillisWrapping) {
  UsbLinkDebounce link;
  const unsigned long nearWrap = static_cast<unsigned long>(-1) - 100;
  ASSERT_TRUE(link.update(true, nearWrap));
  EXPECT_FALSE(link.update(false, nearWrap + 50));
  EXPECT_FALSE(link.update(false, nearWrap + 150));  // wrapped, 100 ms in
  EXPECT_TRUE(link.connected());
  EXPECT_TRUE(link.update(false, nearWrap + 50 + UsbLinkDebounce::kDetachHoldMs));
  EXPECT_FALSE(link.connected());
}

}  // namespace
