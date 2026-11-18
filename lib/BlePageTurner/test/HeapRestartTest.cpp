// When a heap in pieces keeps the radio off, and the one restart it may cost.
// Moved from the app's ble_heap_restart suite; since 29/09/2026 a rollback right after the stack
// start counts like a refusal in pieces.

#include <gtest/gtest.h>

#include "RadioPolicy.h"

namespace bleturner {
namespace {

// X3, 27/09/2026: the heap the radio was refused with, in the book and on Home alike.
constexpr size_t kFree = 87308;
constexpr size_t kLargest = 23540;

// The verdict that ends a start, before the stack comes up and right after it.
Why before(const size_t freeBytes, const size_t largest) {
  RadioInputs in{};
  in.phase = Phase::BeforeStart;
  in.heap = {freeBytes, largest};
  return radioVerdict(in);
}
Why after(const size_t freeBytes, const size_t largest) {
  RadioInputs in{};
  in.phase = Phase::JustStarted;
  in.heap = {freeBytes, largest};
  return radioVerdict(in);
}

TEST(BleHeapRestartDecisionTest, ThirdFragmentedRefusalInARowRestarts) {
  EXPECT_FALSE(shouldRestart(before(kFree, kLargest), 1, false));
  EXPECT_FALSE(shouldRestart(before(kFree, kLargest), 2, false));
  EXPECT_TRUE(shouldRestart(before(kFree, kLargest), 3, false));
  EXPECT_TRUE(shouldRestart(before(kFree, kLargest), 4, false));
}

TEST(BleHeapRestartDecisionTest, HeapThatIsShortInTotalDoesNotRestart) {
  // A restart cannot give back bytes the book really uses.
  EXPECT_FALSE(shouldRestart(before(kMinimumFreeBytes - 1, kLargest), 3, false));
  EXPECT_TRUE(shouldRestart(before(kMinimumFreeBytes, kLargest), 3, false));
}

TEST(BleHeapRestartDecisionTest, WholeLargestBlockDoesNotRestart) {
  EXPECT_FALSE(shouldRestart(before(kFree, kMinimumLargestBlockBytes), 3, false));
  EXPECT_TRUE(shouldRestart(before(kFree, kMinimumLargestBlockBytes - 1), 3, false));
}

TEST(BleHeapRestartDecisionTest, OneRestartUntilTheRadioComesUp) {
  EXPECT_FALSE(shouldRestart(before(kFree, kLargest), 3, true));
  EXPECT_FALSE(shouldRestart(before(kFree, kLargest), 200, true));
}

TEST(BleHeapRestartTrackerTest, ShortHeapBreaksTheStreak) {
  Memo memo{0};
  Tracker tracker{memo};
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kMinimumFreeBytes - 1, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_TRUE(tracker.refused(before(kFree, kLargest)));
}

// X3, 29/09/2026, the 5,000-chapter book: the check before the start passed, the stack left
// largest 26,612. Three of those in a row ask for the restart, as three refusals do.
TEST(BleHeapRestartTrackerTest, RollbacksInPiecesCountLikeRefusals) {
  EXPECT_EQ(after(28812, 26612), Why::HeapInPieces);
  Memo memo{0};
  Tracker tracker{memo};
  EXPECT_FALSE(tracker.refused(after(28812, 26612)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_TRUE(tracker.refused(after(28812, 26612)));
}

TEST(BleHeapRestartTrackerTest, AStartEndedForAnotherReasonBreaksTheStreak) {
  // The stack failed to start, or the start was cancelled: nothing about the heap.
  Memo memo{0};
  Tracker tracker{memo};
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  tracker.inconclusive();
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_TRUE(tracker.refused(before(kFree, kLargest)));
}

// The RTC memo is the only state a restart keeps; a new Tracker over the same memo is the next boot.
TEST(BleHeapRestartTrackerTest, OneRestartSurvivesARestartUntilTheRadioComesUp) {
  Memo memo{0};
  {
    Tracker boot1{memo};
    EXPECT_FALSE(boot1.refused(before(kFree, kLargest)));
    EXPECT_FALSE(boot1.refused(before(kFree, kLargest)));
    ASSERT_TRUE(boot1.refused(before(kFree, kLargest)));
    boot1.restarting();
  }
  {
    // Still in pieces after the restart: no second restart, however long it stays so.
    Tracker boot2{memo};
    for (int i = 0; i < 300; ++i) EXPECT_FALSE(boot2.refused(before(kFree, kLargest))) << i;
  }
  {
    Tracker boot3{memo};
    EXPECT_FALSE(boot3.refused(before(kFree, kLargest)));
    boot3.radioUp();
    // The radio came up: the next time the heap breaks up, the safety net is there again.
    EXPECT_FALSE(boot3.refused(before(kFree, kLargest)));
    EXPECT_FALSE(boot3.refused(before(kFree, kLargest)));
    EXPECT_TRUE(boot3.refused(before(kFree, kLargest)));
  }
}

// Asked in one book, then Home and another book whose start fails for another reason: that is
// not fragmentation and must not restart the device.
TEST(BleHeapRestartTrackerTest, AStartEndedForAnotherReasonDropsAnEarlierRequest) {
  Memo memo{0};
  Tracker tracker{memo};
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
  ASSERT_TRUE(tracker.refused(before(kFree, kLargest)));
  EXPECT_TRUE(tracker.wanted.load());
  tracker.inconclusive();
  EXPECT_FALSE(tracker.wanted.load());
}

// Asked in one book, then another book refused for a heap short in total: a restart cannot give
// back bytes that book really uses, so the request is dropped.
TEST(BleHeapRestartTrackerTest, HeapShortInTotalDropsAnEarlierRequest) {
  Memo memo{0};
  Tracker tracker{memo};
  for (int i = 0; i < 3; ++i) tracker.refused(before(kFree, kLargest));
  ASSERT_TRUE(tracker.wanted.load());
  EXPECT_FALSE(tracker.refused(before(kMinimumFreeBytes - 1, kLargest)));
  EXPECT_FALSE(tracker.wanted.load());
}

TEST(BleHeapRestartTrackerTest, RestartingClearsTheRequest) {
  Memo memo{0};
  Tracker tracker{memo};
  for (int i = 0; i < 3; ++i) tracker.refused(before(kFree, kLargest));
  ASSERT_TRUE(tracker.wanted.load());
  tracker.restarting();
  EXPECT_FALSE(tracker.wanted.load());
}

TEST(BleHeapRestartTrackerTest, ColdBootGarbageInTheMemoIsNotARestart) {
  for (const uint32_t garbage : {0u, 0xFFFFFFFFu, 0xDEADBEEFu, kRestartSpentMagic ^ 1u}) {
    Memo memo{garbage};
    Tracker tracker{memo};
    EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
    EXPECT_FALSE(tracker.refused(before(kFree, kLargest)));
    EXPECT_TRUE(tracker.refused(before(kFree, kLargest))) << garbage;
  }
}

}  // namespace
}  // namespace bleturner
