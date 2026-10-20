// When the first-open "Indexing" popup redraws its progress bar (src/util/IndexingProgressGate.h).
#include <gtest/gtest.h>
#include <util/IndexingProgressGate.h>

namespace {

// Phase ends at the times a small book's open reaches them: no bar, so its popup is no longer.
TEST(IndexingProgressGate, AnOpenThatEndsSoonDrawsNoBar) {
  IndexingProgressGate gate(1000);
  EXPECT_FALSE(gate.shouldDraw(1300, 35));
  EXPECT_FALSE(gate.shouldDraw(1500, 65));
  EXPECT_FALSE(gate.shouldDraw(1700, 85));
  EXPECT_FALSE(gate.drawn());
}

// King's Avatar's phase ends on the X3 (2026-10-20, ms after the popup frame: OPF 1130, TOC 3155,
// book.bin 4373): one bar, at the TOC. A second one at book.bin would cost its ~0.6 s refresh for
// the ~0.7 s of work left.
TEST(IndexingProgressGate, KingsAvatarOnTheX3GetsOneBar) {
  IndexingProgressGate gate(0);
  EXPECT_FALSE(gate.shouldDraw(1130, 35)) << "before the popup has been up two seconds";
  EXPECT_TRUE(gate.shouldDraw(3155, 65));
  EXPECT_FALSE(gate.shouldDraw(4373, 85)) << "within two seconds of the last draw";
  EXPECT_TRUE(gate.drawn());
}

// A longer build still sees the bar move, once each phase ends two seconds or more after the last.
TEST(IndexingProgressGate, ALongerOpenMovesTheBarEveryTwoSecondsAtMost) {
  IndexingProgressGate gate(0);
  EXPECT_TRUE(gate.shouldDraw(2500, 35));
  EXPECT_FALSE(gate.shouldDraw(4400, 65));
  EXPECT_TRUE(gate.shouldDraw(4600, 85));
}

TEST(IndexingProgressGate, TheBarNeverMovesBackOrRedrawsTheSamePercent) {
  IndexingProgressGate gate(0);
  EXPECT_TRUE(gate.shouldDraw(5000, 60));
  EXPECT_FALSE(gate.shouldDraw(7000, 60));
  EXPECT_FALSE(gate.shouldDraw(9000, 30));
  EXPECT_TRUE(gate.shouldDraw(9000, 61));
}

// millis() wraps after 49.7 days; the time since the popup is still right across it.
TEST(IndexingProgressGate, AMillisWrapDoesNotHoldTheBarBack) {
  IndexingProgressGate gate(0xFFFFF000u);
  EXPECT_FALSE(gate.shouldDraw(0xFFFFF700u, 35));  // 1792 ms after the popup, before the wrap
  EXPECT_TRUE(gate.shouldDraw(0x00000100u, 65));   // 4352 ms after it, past the wrap
}

}  // namespace
