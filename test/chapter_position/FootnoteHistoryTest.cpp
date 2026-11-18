#include <gtest/gtest.h>

#include "src/activities/reader/FootnoteHistory.h"

namespace {

void expectPosition(const FootnoteHistory::Position position, const int spine, const int page) {
  EXPECT_EQ(position.spineIndex, spine);
  EXPECT_EQ(position.pageNumber, page);
}

}  // namespace

TEST(FootnoteHistoryTest, ReturnLinkClearsStaleHistory) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});
  history.push({38, 0});
  history.resolveLinkTarget({1, 12});

  EXPECT_TRUE(history.empty());
  EXPECT_FALSE(history.pop().has_value());
}

TEST(FootnoteHistoryTest, RestoredHistoryKeepsReturnPositionOnOrdinaryRender) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});

  FootnoteHistory restored;
  ASSERT_TRUE(restored.restore(history.data(), history.depth()));
  restored.resolveLinkTarget({1, 12});
  ASSERT_EQ(restored.depth(), 1);
  const auto previous = restored.pop();
  ASSERT_TRUE(previous.has_value());
  expectPosition(*previous, 1, 12);
}

TEST(FootnoteHistoryTest, BackButtonClearsHistory) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});

  const auto previous = history.pop();
  ASSERT_TRUE(previous.has_value());
  expectPosition(*previous, 1, 12);
  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, NestedReturnLinkKeepsOuterReturnPosition) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});
  history.push({38, 0});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({38, 0});

  EXPECT_EQ(history.depth(), 1);
  const auto previous = history.pop();
  ASSERT_TRUE(previous.has_value());
  expectPosition(*previous, 1, 12);
}

TEST(FootnoteHistoryTest, ReturnToMainTextClearsAllNestedNotes) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});
  history.push({38, 0});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({1, 12});

  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, ReturnAtDepthLimitStillClearsHistory) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});
  history.push({38, 0});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({38, 10});
  history.push({38, 10});
  history.resolveLinkTarget({1, 12});

  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, PageTurnsDoNotCompleteLinkReturns) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.resolveLinkTarget({1, 12});

  EXPECT_EQ(history.depth(), 1);
  expectPosition(history.data()[0], 1, 12);
}

TEST(FootnoteHistoryTest, SamePageInAnotherSpineIsNotAReturn) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 12});

  EXPECT_EQ(history.depth(), 1);
  expectPosition(history.data()[0], 1, 12);
}

TEST(FootnoteHistoryTest, BacklinkToAnotherPageInMainSpineCompletesReturn) {
  FootnoteHistory history;
  history.push({22, 8});
  history.resolveLinkTarget({38, 20});
  history.push({38, 20});
  history.resolveLinkTarget({22, 9});

  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, SameSpineNotesKeepHistoryUntilReturningToOriginPage) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.push({1, 20});
  history.resolveLinkTarget({1, 30});

  EXPECT_EQ(history.depth(), 2);
  expectPosition(history.data()[0], 1, 12);

  history.push({1, 30});
  history.resolveLinkTarget({1, 12});
  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, FourthJumpKeepsThreeNewestReturnPositions) {
  FootnoteHistory history;
  for (int spine = 1; spine <= 4; ++spine) {
    history.push({spine, spine * 10});
    history.resolveLinkTarget({spine + 1, 0});
  }
  ASSERT_EQ(history.depth(), 3);
  for (int spine = 4; spine >= 2; --spine) {
    const auto previous = history.pop();
    ASSERT_TRUE(previous.has_value());
    expectPosition(*previous, spine, spine * 10);
  }
  EXPECT_FALSE(history.pop().has_value());
}

TEST(FootnoteHistoryTest, RestoredHistoryCanCompleteANewBacklink) {
  const FootnoteHistory::Position positions[] = {{1, 12}, {38, 0}};
  FootnoteHistory history;
  ASSERT_TRUE(history.restore(positions, 2));
  history.push({38, 5});
  history.resolveLinkTarget({1, 13});
  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, InvalidRestoredDepthLeavesHistoryIntact) {
  FootnoteHistory history;
  history.push({1, 12});
  const FootnoteHistory::Position positions[] = {{38, 0}};
  EXPECT_FALSE(history.restore(positions, 4));
  EXPECT_FALSE(history.restore(nullptr, 1));
  ASSERT_EQ(history.depth(), 1);
  expectPosition(history.data()[0], 1, 12);
}

TEST(FootnoteHistoryTest, CrossSpineNestedReturnPrefersExactPageOverEarlierSpine) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.push({1, 20});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({1, 20});

  ASSERT_EQ(history.depth(), 1);
  expectPosition(*history.pop(), 1, 12);
}

TEST(FootnoteHistoryTest, DuplicateOriginsReturnToNewestExactMatch) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  // Ordinary page turns can revisit the origin without consuming history.
  history.resolveLinkTarget({1, 12});
  history.push({1, 12});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({1, 12});

  ASSERT_EQ(history.depth(), 1);
  expectPosition(*history.pop(), 1, 12);
}

TEST(FootnoteHistoryTest, ExactNestedReturnWinsOverEvictedSpineFallback) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.push({1, 20});
  history.resolveLinkTarget({1, 30});
  history.push({1, 30});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({1, 30});

  ASSERT_EQ(history.depth(), 1);
  expectPosition(*history.pop(), 1, 20);
}

TEST(FootnoteHistoryTest, NearbyPageFallbackStillReturnsToFirstSavedSpine) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.push({1, 20});
  history.resolveLinkTarget({38, 5});
  history.push({38, 5});
  history.resolveLinkTarget({1, 13});

  EXPECT_TRUE(history.empty());
}

TEST(FootnoteHistoryTest, DiscardPendingLinkLeavesNoHistoryForFirstJump) {
  FootnoteHistory history;
  history.push({1, 12});
  history.discardPendingLink();

  EXPECT_TRUE(history.empty());
  EXPECT_FALSE(history.pop().has_value());
}

TEST(FootnoteHistoryTest, DiscardPendingLinkPreservesEarlierReturns) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({2, 5});
  history.push({2, 5});
  history.discardPendingLink();

  ASSERT_EQ(history.depth(), 1);
  expectPosition(*history.pop(), 1, 12);
}

TEST(FootnoteHistoryTest, DiscardPendingLinkRestoresEvictedReturnBeforeSaving) {
  FootnoteHistory history;
  for (int spine = 1; spine <= FootnoteHistory::MAX_DEPTH; ++spine) {
    history.push({spine, 12});
    history.resolveLinkTarget({spine + 1, 12});
  }
  history.push({4, 12});
  history.discardPendingLink();
  history.discardPendingLink();

  FootnoteHistory reopened;
  ASSERT_TRUE(reopened.restore(history.data(), history.depth()));
  ASSERT_EQ(reopened.depth(), FootnoteHistory::MAX_DEPTH);
  for (int spine = FootnoteHistory::MAX_DEPTH; spine >= 1; --spine) {
    expectPosition(*reopened.pop(), spine, 12);
  }
}

TEST(FootnoteHistoryTest, DiscardPendingLinkKeepsRenderedJump) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({2, 5});
  history.discardPendingLink();

  ASSERT_EQ(history.depth(), 1);
  expectPosition(*history.pop(), 1, 12);
}
