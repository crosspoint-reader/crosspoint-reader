// DisplayedPage: what a reader render pass records once its page is on the panel, when the loop task may have moved
// the position on while the pass had the lock given up for the waveform.
#include <gtest/gtest.h>

#include "activities/reader/DisplayedPage.h"

namespace {

using DisplayedPage::Identity;
using DisplayedPage::LivePosition;
using DisplayedPage::settle;

constexpr int kSpines = 58;

void expectRecords(const DisplayedPage::Settlement& s, const Identity& page) {
  ASSERT_TRUE(s.recordProgress);
  EXPECT_EQ(page.spine, s.progress.spine);
  EXPECT_EQ(page.page, s.progress.page);
  EXPECT_EQ(page.pageCount, s.progress.pageCount);
}

TEST(DisplayedPage, AnUndisturbedPassRecordsItsPage) {
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {true, 9, 4}, kSpines);
  EXPECT_FALSE(s.superseded);
  expectRecords(s, drawn);
}

TEST(DisplayedPage, ATurnDuringTheWaveformLeavesTheRecordOnTheDrawnPage) {
  // The next page is not on the panel yet; the pass the turn asked for records it once it is.
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {true, 9, 5}, kSpines);
  EXPECT_TRUE(s.superseded);
  expectRecords(s, drawn);
  // A burst: two turns taken in one waveform.
  expectRecords(settle(drawn, {true, 9, 6}, kSpines), drawn);
  expectRecords(settle(drawn, {true, 9, 3}, kSpines), drawn);
}

TEST(DisplayedPage, ATurnAcrossASpineBoundaryStillRecordsTheChaptersLastPage) {
  // The turn reset the section: the pass relocks to no section at all. The page it drew is the chapter's last, which
  // nothing else will record.
  const Identity drawn{3, 2, 3};
  const auto forward = settle(drawn, {false, 4, 0}, kSpines);
  EXPECT_TRUE(forward.superseded);
  expectRecords(forward, drawn);

  const Identity first{9, 0, 21};
  const auto back = settle(first, {false, 8, 0}, kSpines);
  EXPECT_TRUE(back.superseded);
  expectRecords(back, first);
}

TEST(DisplayedPage, AJumpThatReloadsTheSameSpineIsSupersededToo) {
  // A link into the same chapter resets the section without changing the spine.
  const Identity drawn{9, 4, 21};
  const auto s = settle(drawn, {false, 9, 0}, kSpines);
  EXPECT_TRUE(s.superseded);
  expectRecords(s, drawn);
}

TEST(DisplayedPage, ATurnPastTheLastPageLeavesTheRecordToTheFinishedBookFlow) {
  // The finished-book pass writes its 100% record directly; a page record published here would land after it.
  const Identity last{kSpines - 1, 11, 12};
  const auto s = settle(last, {false, kSpines, 0}, kSpines);
  EXPECT_TRUE(s.superseded);
  EXPECT_FALSE(s.recordProgress);
  // The book's last page itself, undisturbed, is recorded as usual.
  expectRecords(settle(last, {true, kSpines - 1, 11}, kSpines), last);
}

}  // namespace
