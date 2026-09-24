#include <gtest/gtest.h>

#include "activities/reader/PendingPageTurn.h"

TEST(PendingPageTurn, DeferredTurnRunsOnceAfterRenderCompletes) {
  ReaderUtils::PendingPageTurn pending;
  pending.enqueue(1);
  EXPECT_EQ(pending.take(false), 0);
  EXPECT_TRUE(pending.hasPending());

  pending.requestRender();
  EXPECT_EQ(pending.take(true), 0);  // Render requested but not started.
  const auto ticket = pending.beginRender();
  EXPECT_EQ(pending.take(true), 0);
  pending.endRender(ticket);

  EXPECT_EQ(pending.take(true), 1);  // No new input is needed to drain the turn.
  EXPECT_EQ(pending.take(true), 0);
}

TEST(PendingPageTurn, RepeatedInputsCoalesceAndClearCancelsPendingTurn) {
  ReaderUtils::PendingPageTurn pending;
  pending.enqueue(1);
  pending.enqueue(1);
  EXPECT_EQ(pending.take(true), 1);
  EXPECT_EQ(pending.take(true), 0);

  pending.enqueue(1);
  pending.enqueue(-1);
  EXPECT_EQ(pending.take(true), -1);

  pending.enqueue(1);
  pending.clear();
  EXPECT_EQ(pending.take(true), 0);
}

TEST(PendingPageTurn, EarlierRenderCannotCompleteANewerRequest) {
  ReaderUtils::PendingPageTurn pending;
  pending.requestRender();
  const auto oldRender = pending.beginRender();
  pending.requestRender();
  pending.enqueue(-1);

  pending.endRender(oldRender);
  EXPECT_EQ(pending.take(true), 0);
  pending.endRender(pending.beginRender());
  EXPECT_EQ(pending.take(true), -1);
}
