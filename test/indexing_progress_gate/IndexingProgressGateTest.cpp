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

// King's Avatar's phase ends on the X3 after the fix (OPF ~0.9 s, TOC ~0.8 s, book.bin ~0.6 s):
// the bar appears once the popup has been up for two seconds, and moves again a second on.
TEST(IndexingProgressGate, ALongOpenDrawsTheBarAtPhaseEndsAfterTwoSeconds) {
  IndexingProgressGate gate(0);
  EXPECT_FALSE(gate.shouldDraw(900, 35));
  EXPECT_TRUE(gate.shouldDraw(2100, 65));
  EXPECT_FALSE(gate.shouldDraw(2700, 85)) << "within a second of the last draw";
  EXPECT_TRUE(gate.shouldDraw(3200, 90));
  EXPECT_TRUE(gate.drawn());
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
