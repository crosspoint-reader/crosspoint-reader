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

// Between the turn that left a chapter and the render task starting the next one, no chapter is loaded.
TEST(QueuedTurns, CarriedTurnsWaitBrieflyForTheNextChapter) {
  EXPECT_TRUE(queuedTurnsAwaitSection(0));
  EXPECT_TRUE(queuedTurnsAwaitSection(250));
  EXPECT_TRUE(queuedTurnsAwaitSection(kQueuedTurnsSectionWaitMs - 1));
}

TEST(QueuedTurns, AChapterThatNeverLoadsDropsThem) {
  EXPECT_FALSE(queuedTurnsAwaitSection(kQueuedTurnsSectionWaitMs));
  EXPECT_FALSE(queuedTurnsAwaitSection(60000));
}

namespace {

// One frame of the reader's loop as far as the queue goes: look at the queue, then, unless that
// used the frame up, read a page-turn press. With no chapter loaded a press counts toward turns
// that are already waiting, as it does in EpubReaderActivity::loop().
struct LoopFrame {
  int8_t pending = 0;
  bool turnGuardActive = false;
  bool sectionLoaded = false;
  unsigned long msSinceTurn = 0;
  int press = 0;  // +1 forward, -1 back, 0 none
  int8_t carriedByApply = 0;

  int inputReads = 0;
  int applyCalls = 0;
  int8_t applied = 0;

  void run() {
    const auto apply = [this](const int8_t turns) {
      ++applyCalls;
      applied = turns;
      return carriedByApply;
    };
    if (drainQueuedTurns(pending, turnGuardActive, sectionLoaded, msSinceTurn, apply) == QueuedTurnsPass::Done) return;
    ++inputReads;
    if (press == 0) return;
    if (!sectionLoaded && pending != 0) pending = queueTurn(pending, press > 0);
  }
};

}  // namespace

// 300 ms after the turn that left the chapter, nothing loaded yet, one forward turn carried.
TEST(QueuedTurnsDrain, ABackPressCancelsATurnWaitingForTheNextChapter) {
  LoopFrame frame;
  frame.pending = 1;
  frame.msSinceTurn = 300;
  frame.press = -1;
  frame.run();
  EXPECT_EQ(frame.inputReads, 1);
  EXPECT_EQ(frame.applyCalls, 0);
  EXPECT_EQ(frame.pending, 0);
}

TEST(QueuedTurnsDrain, AForwardPressAddsToTurnsWaitingForTheNextChapter) {
  LoopFrame frame;
  frame.pending = 2;
  frame.msSinceTurn = 300;
  frame.press = +1;
  frame.run();
  EXPECT_EQ(frame.inputReads, 1);
  EXPECT_EQ(frame.pending, 3);
}

TEST(QueuedTurnsDrain, WaitingTurnsStayWhenNothingIsPressed) {
  LoopFrame frame;
  frame.pending = 3;
  frame.msSinceTurn = 300;
  frame.run();
  EXPECT_EQ(frame.inputReads, 1);
  EXPECT_EQ(frame.pending, 3);
}

TEST(QueuedTurnsDrain, TurnsAreDroppedOnceTheWaitForAChapterRunsOut) {
  LoopFrame frame;
  frame.pending = 3;
  frame.msSinceTurn = kQueuedTurnsSectionWaitMs;
  frame.press = +1;
  frame.run();
  EXPECT_EQ(frame.inputReads, 0);
  EXPECT_EQ(frame.applyCalls, 0);
  EXPECT_EQ(frame.pending, 0);
}

TEST(QueuedTurnsDrain, ALoadedChapterTakesTheQueueAndKeepsWhatIsCarried) {
  LoopFrame frame;
  frame.pending = 4;
  frame.sectionLoaded = true;
  frame.msSinceTurn = 300;
  frame.carriedByApply = 3;
  frame.run();
  EXPECT_EQ(frame.applyCalls, 1);
  EXPECT_EQ(frame.applied, 4);
  EXPECT_EQ(frame.inputReads, 0);
  EXPECT_EQ(frame.pending, 3);
}

TEST(QueuedTurnsDrain, ABusyPanelLeavesTheQueueAndReadsInput) {
  LoopFrame frame;
  frame.pending = 2;
  frame.turnGuardActive = true;
  frame.sectionLoaded = true;
  frame.run();
  EXPECT_EQ(frame.applyCalls, 0);
  EXPECT_EQ(frame.inputReads, 1);
  EXPECT_EQ(frame.pending, 2);
}

TEST(QueuedTurnsDrain, AnEmptyQueueReadsInput) {
  LoopFrame frame;
  frame.sectionLoaded = true;
  frame.run();
  EXPECT_EQ(frame.applyCalls, 0);
  EXPECT_EQ(frame.inputReads, 1);
}
