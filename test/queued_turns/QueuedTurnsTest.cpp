#include <gtest/gtest.h>

#include "activities/reader/QueuedTurns.h"

TEST(QueuedTurns, CountsPressesUpToFourEitherWay) {
  EXPECT_EQ(queueTurn(0, true), 1);
  EXPECT_EQ(queueTurn(3, true), 4);
  EXPECT_EQ(queueTurn(4, true), 4);
  EXPECT_EQ(queueTurn(0, false), -1);
  EXPECT_EQ(queueTurn(-4, false), -4);
}

TEST(QueuedTurns, AnOppositePressCancelsOne) {
  EXPECT_EQ(queueTurn(2, false), 1);
  EXPECT_EQ(queueTurn(-1, true), 0);
}

TEST(QueuedTurns, AFinishedChapterTakesThemAll) {
  const auto turns = splitQueuedForwardTurns(4, false, 8, 10);
  EXPECT_EQ(turns.now, 4);
  EXPECT_EQ(turns.held, 0);
}

TEST(QueuedTurns, ABuildingChapterTakesThemAllWhileThePagesExist) {
  const auto turns = splitQueuedForwardTurns(3, true, 2, 10);
  EXPECT_EQ(turns.now, 3);
  EXPECT_EQ(turns.held, 0);
}

// Page index 8 of 10 laid out so far, four presses queued: one lands on page 9, one steps onto the
// page still to be built, and two wait to see whether the chapter ends there.
TEST(QueuedTurns, ABuildingChapterHoldsTurnsPastItsFirstUnbuiltPage) {
  const auto turns = splitQueuedForwardTurns(4, true, 8, 10);
  EXPECT_EQ(turns.now, 2);
  EXPECT_EQ(turns.held, 2);
}

TEST(QueuedTurns, OnTheLastBuiltPageOnlyOneTurnGoesAhead) {
  const auto turns = splitQueuedForwardTurns(4, true, 9, 10);
  EXPECT_EQ(turns.now, 1);
  EXPECT_EQ(turns.held, 3);
}

TEST(QueuedTurns, AlreadyPastTheBuiltPagesStillTakesOnlyOne) {
  const auto turns = splitQueuedForwardTurns(3, true, 10, 10);
  EXPECT_EQ(turns.now, 1);
  EXPECT_EQ(turns.held, 2);
}

TEST(QueuedTurns, HeldTurnsCarryOverOnceTheRestAreApplied) { EXPECT_EQ(queuedTurnsToCarry(0, 2, false), 2); }

// Four queued on a chapter's last page: one crosses into the next chapter, three follow after it loads.
TEST(QueuedTurns, TurnsLeftAtAChapterEdgeCarryIntoTheNextChapter) { EXPECT_EQ(queuedTurnsToCarry(3, 0, true), 3); }

TEST(QueuedTurns, AFailedTurnDropsTheOnesBehindIt) { EXPECT_EQ(queuedTurnsToCarry(2, 0, false), 0); }

TEST(QueuedTurns, NothingLeftMeansNothingCarried) {
  EXPECT_EQ(queuedTurnsToCarry(0, 0, false), 0);
  EXPECT_EQ(queuedTurnsToCarry(0, 0, true), 0);
}
