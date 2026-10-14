#include <gtest/gtest.h>

#include "src/activities/reader/FootnoteHistory.h"

namespace {

FootnoteHistory::Position sleepPosition(const FootnoteHistory& history, const FootnoteHistory::Position current) {
  const auto* origin = history.origin();
  return origin ? *origin : current;
}

void expectPosition(const FootnoteHistory::Position position, const int spine, const int page) {
  EXPECT_EQ(position.spineIndex, spine);
  EXPECT_EQ(position.pageNumber, page);
}

}  // namespace

TEST(FootnoteHistoryTest, ReturnLinkThenReadingSavesLatestPageOnSleep) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});
  history.push({38, 0});
  history.resolveLinkTarget({1, 12});

  EXPECT_TRUE(history.empty());
  expectPosition(sleepPosition(history, {3, 25}), 3, 25);
  EXPECT_FALSE(history.pop().has_value());
}

TEST(FootnoteHistoryTest, SleepWhileReadingNoteSavesMainTextOrigin) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});

  EXPECT_EQ(history.depth(), 1);
  expectPosition(sleepPosition(history, {38, 2}), 1, 12);
}

TEST(FootnoteHistoryTest, BackButtonThenReadingSavesLatestPageOnSleep) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 0});

  const auto previous = history.pop();
  ASSERT_TRUE(previous.has_value());
  expectPosition(*previous, 1, 12);
  EXPECT_TRUE(history.empty());
  expectPosition(sleepPosition(history, {3, 25}), 3, 25);
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
  expectPosition(sleepPosition(history, {38, 0}), 1, 12);
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
  expectPosition(sleepPosition(history, {2, 25}), 2, 25);
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
  expectPosition(sleepPosition(history, {1, 15}), 1, 12);
}

TEST(FootnoteHistoryTest, SamePageInAnotherSpineIsNotAReturn) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({38, 12});

  EXPECT_EQ(history.depth(), 1);
  expectPosition(sleepPosition(history, {38, 12}), 1, 12);
}

TEST(FootnoteHistoryTest, BacklinkToAnotherPageInMainSpineCompletesReturn) {
  FootnoteHistory history;
  history.push({22, 8});
  history.resolveLinkTarget({38, 20});
  history.push({38, 20});
  history.resolveLinkTarget({22, 9});

  EXPECT_TRUE(history.empty());
  expectPosition(sleepPosition(history, {24, 25}), 24, 25);
}

TEST(FootnoteHistoryTest, SameSpineNotesKeepHistoryUntilReturningToOriginPage) {
  FootnoteHistory history;
  history.push({1, 12});
  history.resolveLinkTarget({1, 20});
  history.push({1, 20});
  history.resolveLinkTarget({1, 30});

  EXPECT_EQ(history.depth(), 2);
  expectPosition(sleepPosition(history, {1, 30}), 1, 12);

  history.push({1, 30});
  history.resolveLinkTarget({1, 12});
  EXPECT_TRUE(history.empty());
  expectPosition(sleepPosition(history, {1, 45}), 1, 45);
}
