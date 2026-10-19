#include <gtest/gtest.h>

#include "activities/reader/ChapterBuildGate.h"

namespace {
bleturner::Status radio() { return bleturner::Status{}; }
}  // namespace

TEST(ChapterBuildGate, RunsWithTheRadioOff) {
  EXPECT_TRUE(chapterBuildMayRun(radio()));
  auto idle = radio();
  idle.idleStopped = true;  // stopped for this build, or for idleness
  EXPECT_TRUE(chapterBuildMayRun(idle));
  auto refused = radio();
  refused.readerDeferred = true;  // refused for memory: it holds nothing
  EXPECT_TRUE(chapterBuildMayRun(refused));
}

// The radio did not stop within its budget (StillUp), or a start was still settling after it
// (NotHeld): it holds or is taking the heap the build needs.
TEST(ChapterBuildGate, RefusedWhileTheRadioHoldsOrTakesTheHeap) {
  auto up = radio();
  up.running = true;
  up.connected = true;
  EXPECT_FALSE(chapterBuildMayRun(up));
  auto stopping = radio();
  stopping.stopping = true;
  EXPECT_FALSE(chapterBuildMayRun(stopping));
  auto starting = radio();
  starting.starting = true;
  EXPECT_FALSE(chapterBuildMayRun(starting));
}

namespace {
// A clock that only moves when the wait sleeps.
struct FakeClock {
  uint32_t nowMs = 1000;
  unsigned asks = 0;
  bool wait(const uint32_t readyAtMs, const uint32_t waitMs) {
    const uint32_t started = nowMs;
    return waitForBuildRoom(
        [&] {
          ++asks;
          return nowMs - started >= readyAtMs;
        },
        [&] { return nowMs; }, [&](const uint32_t ms) { nowMs += ms; }, waitMs, 50);
  }
};
}  // namespace

TEST(ChapterBuildGate, ReadyAtOnceDoesNotWait) {
  FakeClock clock;
  EXPECT_TRUE(clock.wait(0, 5000));
  EXPECT_EQ(clock.nowMs, 1000u);
  EXPECT_EQ(clock.asks, 1u);
}

// The radio stops a little after the module's own 3 s budget: the build waits for it.
TEST(ChapterBuildGate, RadioThatStopsAfterASecondLetsTheBuildRun) {
  FakeClock clock;
  EXPECT_TRUE(clock.wait(1000, 5000));
  EXPECT_GE(clock.nowMs - 1000, 1000u);
  EXPECT_LT(clock.nowMs - 1000, 1050u);
}

TEST(ChapterBuildGate, RadioThatNeverStopsIsRefusedAfterTheWait) {
  FakeClock clock;
  EXPECT_FALSE(clock.wait(UINT32_MAX, 5000));
  EXPECT_GE(clock.nowMs - 1000, 5000u);
  EXPECT_LT(clock.nowMs - 1000, 5050u);
}

// The first ask is the module's own stop, which can take seconds: the extra wait starts after it.
TEST(ChapterBuildGate, TheWaitStartsAfterASlowFirstAsk) {
  uint32_t nowMs = 0;
  unsigned asks = 0;
  const bool ok = waitForBuildRoom(
      [&] {
        if (asks++ == 0) nowMs += 3000;
        return false;
      },
      [&] { return nowMs; }, [&](const uint32_t ms) { nowMs += ms; }, 5000, 50);
  EXPECT_FALSE(ok);
  EXPECT_GE(nowMs, 8000u);
  EXPECT_LT(nowMs, 8050u);
}
