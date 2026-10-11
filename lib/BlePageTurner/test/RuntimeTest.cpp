// The radio start, stop and heap rules of the runtime, against a fake radio and a fake host.
// Moved from the app's ble_page_turner_runtime suite; the numbers and outcomes are the same.

#include <gtest/gtest.h>

#include <cstring>

#include "Fakes.h"
#include "Runtime.h"

namespace {

using bleturner::Config;
using fake::host;
using fake::radio;

constexpr size_t kEnoughFree = 65536;
constexpr size_t kEnoughLargest = 32768;

class RuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    bleturner::begin(fake::hostFns(), config);
    host().heap = {kEnoughFree, kEnoughLargest};
  }
  // A start that was refused for memory leaves "the book's start was deferred".
  void deferReaderStart() {
    const auto heap = host().heap;
    host().heap = {kEnoughFree - 1, kEnoughLargest};
    ASSERT_TRUE(bleturner::detail::startAsync());
    host().heap = heap;
    ASSERT_TRUE(bleturner::status().readerDeferred);
    host().logs.clear();
  }
  Config config;
};

TEST_F(RuntimeTest, LowMemorySkipsHostWithoutInitialization) {
  host().heap.freeBytes = kEnoughFree - 1;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 1u);
  ASSERT_EQ(host().logs.size(), 1u);
  EXPECT_NE(host().logs.front().find("ERR HID begin skipped (insufficient-internal-heap)"), std::string::npos);
  EXPECT_NE(host().logs.front().find("free=65535"), std::string::npos);
  EXPECT_NE(host().logs.front().find("required_free=65536"), std::string::npos);
  EXPECT_EQ(host().maps.size(), 1u) << "a refusal prints the heap map (twice per boot at most)";
}

TEST_F(RuntimeTest, FragmentedLargestBlockSkipsHostWithoutInitialization) {
  host().heap.largestBlock = kEnoughLargest - 1;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 1u);
  ASSERT_EQ(host().logs.size(), 1u);
  EXPECT_NE(host().logs.front().find("largest=32767"), std::string::npos);
}

TEST_F(RuntimeTest, BusyTransferSkipsBeforeCacheReleaseAndHeapProbe) {
  host().transfer = true;
  host().heap = {123, 789};

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 0u);
  EXPECT_EQ(host().heapReads, 1u);
  ASSERT_EQ(host().logs.size(), 1u);
  EXPECT_NE(host().logs.front().find("filetransfer-active"), std::string::npos);
  EXPECT_NE(host().logs.front().find("free=123"), std::string::npos);
}

// A file transfer takes the radio with a 1 s stop (FileTransferState.h in the app).
TEST_F(RuntimeTest, TimedStopPropagatesPendingTeardown) {
  radio().stopping = true;
  radio().endResult = false;

  EXPECT_FALSE(bleturner::beforeScreenChange(1000));
  EXPECT_EQ(radio().endCalls, 1u);
  EXPECT_EQ(radio().lastEndTimeoutMs, 1000u);
}

TEST_F(RuntimeTest, PropagatesHostInitializationFailure) {
  radio().beginResult = false;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_EQ(host().releaseCalls, 1u);
  EXPECT_TRUE(radio().beginHadFullSpeed);
  EXPECT_FALSE(host().fullSpeed) << "a failed start gives the CPU back";
}

TEST_F(RuntimeTest, PendingTeardownCannotRestartOrReleaseCaches) {
  radio().stopping = true;
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 0u);
  EXPECT_EQ(host().heapReads, 0u);
}

TEST_F(RuntimeTest, SuccessfulStackWithoutReaderHeadroomIsRolledBack) {
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {50000, kEnoughLargest - 1};

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_EQ(radio().endCalls, 1u);
  EXPECT_EQ(radio().lastEndTimeoutMs, 0u);
  EXPECT_FALSE(radio().running);
  ASSERT_EQ(host().logs.size(), 1u);
  EXPECT_NE(host().logs.front().find("post-init-headroom"), std::string::npos);
}

TEST_F(RuntimeTest, ReaderHeadroomRollbackCanRemainPendingWithoutRestarting) {
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {50000, kEnoughLargest - 1};
  radio().endResult = false;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(radio().stopping);
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_EQ(radio().endCalls, 1u);
}

TEST_F(RuntimeTest, ExactReaderBlockAfterStackStartIsAccepted) {
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {40000, kEnoughLargest};

  EXPECT_TRUE(bleturner::switchOn());
  EXPECT_TRUE(radio().running);
  EXPECT_EQ(radio().endCalls, 0u);
  EXPECT_TRUE(host().fullSpeed) << "a running radio keeps the CPU at full speed";
}

TEST_F(RuntimeTest, RunningHostReturnsTrueWithoutRetryOrCacheChurn) {
  ASSERT_TRUE(bleturner::switchOn());
  ASSERT_EQ(radio().beginCalls, 1u);
  ASSERT_EQ(host().releaseCalls, 1u);
  const unsigned readsAfterFirstBegin = host().heapReads;

  EXPECT_TRUE(bleturner::switchOn());
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_EQ(host().releaseCalls, 1u);
  EXPECT_EQ(host().heapReads, readsAfterFirstBegin);
}

TEST_F(RuntimeTest, ScreenChangeDefersUntilRadioMemoryIsReleased) {
  radio().running = true;
  radio().endResult = false;
  EXPECT_FALSE(bleturner::beforeScreenChange());
  EXPECT_EQ(radio().lastEndTimeoutMs, 0u);
  EXPECT_TRUE(radio().stopping);
  EXPECT_TRUE(radio().endHadFullSpeed);

  EXPECT_FALSE(bleturner::beforeScreenChange());
  radio().endResult = true;
  EXPECT_TRUE(bleturner::beforeScreenChange());
  EXPECT_FALSE(radio().stopping);
  EXPECT_EQ(radio().endCalls, 3u);
  EXPECT_FALSE(host().fullSpeed);
}

TEST_F(RuntimeTest, ScreenChangeWithIdleRadioDoesNotAllocateOrStopAgain) {
  EXPECT_TRUE(bleturner::beforeScreenChange());
  EXPECT_EQ(radio().endCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 0u);
  EXPECT_EQ(host().heapReads, 0u);
}

TEST_F(RuntimeTest, IdleStopRecordsReasonAndClearsStaleReaderFailure) {
  deferReaderStart();
  radio().running = true;

  EXPECT_TRUE(bleturner::detail::stopForIdle());
  EXPECT_TRUE(bleturner::status().idleStopped);
  EXPECT_FALSE(bleturner::status().readerDeferred);
  EXPECT_FALSE(radio().running);
  EXPECT_EQ(radio().endCalls, 1u);
  EXPECT_EQ(radio().lastEndTimeoutMs, 0u);
}

TEST_F(RuntimeTest, PendingIdleStopKeepsReasonThroughCleanupAndNavigation) {
  radio().running = true;
  radio().endResult = false;

  EXPECT_FALSE(bleturner::detail::stopForIdle());
  EXPECT_TRUE(bleturner::status().idleStopped);
  EXPECT_TRUE(radio().stopping);
  EXPECT_FALSE(bleturner::beforeScreenChange());
  EXPECT_TRUE(bleturner::status().idleStopped);

  radio().endResult = true;
  EXPECT_TRUE(bleturner::beforeScreenChange());
  EXPECT_FALSE(radio().stopping);
  EXPECT_TRUE(bleturner::status().idleStopped);
}

TEST_F(RuntimeTest, ExplicitStartClearsReasonsEvenWhenMemoryCheckRejectsIt) {
  ASSERT_TRUE(bleturner::detail::stopForIdle());
  deferReaderStart();
  ASSERT_FALSE(bleturner::status().idleStopped) << "the deferred start cleared it; set it again";
  ASSERT_TRUE(bleturner::detail::stopForIdle());
  deferReaderStart();
  host().heap.freeBytes = kEnoughFree - 1;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::status().idleStopped);
  EXPECT_FALSE(bleturner::status().readerDeferred);
}

TEST_F(RuntimeTest, ExplicitStartDuringCleanupClearsOldIdleReason) {
  ASSERT_TRUE(bleturner::detail::stopForIdle());
  radio().stopping = true;

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::status().idleStopped);
  EXPECT_EQ(radio().beginCalls, 0u);
}

TEST_F(RuntimeTest, ExplicitAsyncStartRestartsAfterIdleStop) {
  radio().running = true;
  ASSERT_TRUE(bleturner::detail::stopForIdle());

  EXPECT_TRUE(bleturner::detail::startAsync());
  EXPECT_TRUE(radio().running);
  EXPECT_FALSE(bleturner::status().idleStopped);
  EXPECT_EQ(radio().beginCalls, 1u);
}

TEST_F(RuntimeTest, QueuedStartCancelledByScreenChangeNeverBegins) {
  radio().queueStarts = true;
  ASSERT_TRUE(bleturner::detail::startAsync());
  EXPECT_TRUE(host().fullSpeed);
  EXPECT_TRUE(bleturner::status().starting);
  EXPECT_FALSE(bleturner::beforeScreenChange()) << "never stop while a start is in flight";
  EXPECT_EQ(radio().endCalls, 0u);
  radio().runStart();
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_FALSE(bleturner::status().readerDeferred) << "a cancelled start is not a memory refusal";
  EXPECT_TRUE(bleturner::beforeScreenChange());
  EXPECT_FALSE(host().fullSpeed);
}

TEST_F(RuntimeTest, OneStartAtATime) {
  radio().queueStarts = true;
  ASSERT_TRUE(bleturner::detail::startAsync());
  EXPECT_FALSE(bleturner::detail::startAsync());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_EQ(radio().creates, 1u);
  radio().runStart();
  EXPECT_EQ(radio().beginCalls, 1u);
}

TEST_F(RuntimeTest, TaskCreationFailureReleasesTheStart) {
  radio().createSucceeds = false;
  EXPECT_FALSE(bleturner::detail::startAsync());
  EXPECT_FALSE(host().fullSpeed);
  EXPECT_FALSE(bleturner::status().starting);
  EXPECT_TRUE(bleturner::status().readerDeferred);
}

// The start waits for the render lock (a paint in flight) instead of taking the heap check
// with the caches still full.
TEST_F(RuntimeTest, StartWaitsForTheRenderLockInShortSteps) {
  host().renderBusy = true;
  int turns = 0;
  radio().onSleep = [&] {
    if (++turns == 3) host().renderBusy = false;
  };
  EXPECT_TRUE(bleturner::switchOn());
  EXPECT_EQ(turns, 3);
  EXPECT_EQ(radio().now, 10030u);
  EXPECT_EQ(host().releaseCalls, 1u);
}

// X3, 27/09/2026: the radio refused every 5 s for good with free=87308 largest=23540.
TEST_F(RuntimeTest, ThirdFragmentedRefusalAsksForOneRestartUntilTheRadioComesUp) {
  ASSERT_TRUE(bleturner::switchOn());  // a radio that came up clears any earlier streak
  radio().running = false;
  host().heap = {87308, 23540};

  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(bleturner::detail::heapRestartWanted());
  EXPECT_EQ(radio().beginCalls, 1u);

  // The tick restarts into the book from a shown page; the heap it finds after is still in pieces.
  bleturner::tick(fake::reading());
  EXPECT_EQ(host().restarts, 1u);
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());

  fake::reset(/*keepMemo=*/true);
  bleturner::begin(fake::hostFns(), config);
  host().heap = {87308, 23540};
  for (int i = 0; i < 6; ++i) EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());

  host().heap = {kEnoughFree, kEnoughLargest};
  ASSERT_TRUE(bleturner::switchOn());
  radio().running = false;
  host().heap = {87308, 23540};
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(bleturner::detail::heapRestartWanted());
}

TEST_F(RuntimeTest, ShortHeapNeverAsksForARestart) {
  ASSERT_TRUE(bleturner::switchOn());
  radio().running = false;
  host().heap = {kEnoughFree - 1, 23540};
  for (int i = 0; i < 5; ++i) EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());
}

// X3, 29/09/2026, the 5,000-chapter book: the heap check passed every time (free about 80 KB,
// largest 61,428), the stack came up and left largest 26,612, so every start rolled back, 7 of
// 7, and the restart that rescued the same book on 27/09 never came.
TEST_F(RuntimeTest, PostInitRollbackInPiecesAsksForOneRestartUntilTheRadioComesUp) {
  host().heap = {80000, 61428};
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {28812, 26612};
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(bleturner::detail::heapRestartWanted());
  EXPECT_EQ(radio().beginCalls, 3u);
  EXPECT_NE(
      host().logs.back().find("rolled back (post-init-headroom): free=28812 largest=26612 required_largest=32768"),
      std::string::npos);

  // Restarted into the book; the same heap again: no second restart until the radio comes up.
  bleturner::tick(fake::reading());
  EXPECT_EQ(host().restarts, 1u);
  fake::reset(/*keepMemo=*/true);
  bleturner::begin(fake::hostFns(), config);
  host().heap = {80000, 61428};
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {28812, 26612};
  for (int i = 0; i < 6; ++i) EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());
}

// Refusals before the start and rollbacks after it are one streak; a start the stack itself
// failed says nothing about the heap and breaks it.
TEST_F(RuntimeTest, RefusalsAndRollbacksInPiecesCountTogetherAndAStackFailureBreaksTheStreak) {
  host().heap = {87308, 23540};
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::switchOn());
  host().heap = {80000, 61428};
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {28812, 26612};
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(bleturner::detail::heapRestartWanted());

  fake::reset();
  bleturner::begin(fake::hostFns(), config);
  host().heap = {80000, 61428};
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {28812, 26612};
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::switchOn());
  radio().beginResult = false;
  EXPECT_FALSE(bleturner::switchOn());
  radio().beginResult = true;
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_FALSE(bleturner::detail::heapRestartWanted());
  EXPECT_FALSE(bleturner::switchOn());
  EXPECT_TRUE(bleturner::detail::heapRestartWanted());
}

TEST_F(RuntimeTest, SleepWaitsForTheStopUpToItsTimeout) {
  radio().running = true;
  radio().endResult = false;
  EXPECT_FALSE(bleturner::beforeSleep(100));
  EXPECT_GE(radio().now, 10100u);
  int turns = 0;
  radio().onSleep = [&] {
    if (++turns == 2) radio().endResult = true;
  };
  EXPECT_TRUE(bleturner::beforeSleep(1000));
  EXPECT_FALSE(radio().running || radio().stopping);
}

TEST_F(RuntimeTest, SwitchOffClearsReasonsAndStopsWithOneSecond) {
  ASSERT_TRUE(bleturner::detail::stopForIdle());
  radio().running = true;
  EXPECT_TRUE(bleturner::switchOff());
  EXPECT_EQ(radio().lastEndTimeoutMs, 1000u);
  EXPECT_FALSE(bleturner::status().idleStopped);
}

TEST_F(RuntimeTest, PairStartsTheRadioThenConnects) {
  EXPECT_TRUE(bleturner::pair("AA:BB:CC:DD:EE:01", "Remote"));
  EXPECT_TRUE(radio().running);
  ASSERT_EQ(radio().connects.size(), 1u);
  config.enabled = 0;
  EXPECT_FALSE(bleturner::pair("AA:BB:CC:DD:EE:02", "Remote")) << "a page turner switched off does not pair";
  EXPECT_EQ(radio().connects.size(), 1u);
}

TEST_F(RuntimeTest, ForgetDropsTheBondItsTableAndTheChoice) {
  bleturner::RemoteTable* t = bleturner::editableTable(config.remotes, config.remoteCount, "AA:BB:CC:DD:EE:01", "");
  ASSERT_NE(t, nullptr);
  strcpy(config.peerAddr, "AA:BB:CC:DD:EE:01");
  strcpy(config.peerName, "Remote");
  EXPECT_TRUE(bleturner::forget("AA:BB:CC:DD:EE:01"));
  EXPECT_EQ(config.remoteCount, 0);
  EXPECT_STREQ(config.peerAddr, "");
  EXPECT_STREQ(config.peerName, "");
  ASSERT_EQ(radio().forgotten.size(), 1u);
  EXPECT_FALSE(bleturner::forget("AA:BB:CC:DD:EE:09")) << "nothing saved changes for an unknown remote";
}

TEST_F(RuntimeTest, PollEventDrainsKeysAndHandsOverRawEdges) {
  radio().keys.push_back({0x4F, 0, true});
  bleturner::port::RawEdge e;
  e.reportId = 3;
  e.byteIndex = 1;
  e.value = 2;
  e.pressed = true;
  e.atMs = 42;
  radio().raw.push_back(e);
  bleturner::Event ev{};
  ASSERT_TRUE(bleturner::pollEvent(ev));
  EXPECT_EQ(ev.code, 0x030102u);
  EXPECT_TRUE(ev.pressed);
  EXPECT_EQ(ev.atMs, 42u);
  EXPECT_TRUE(radio().keys.empty());
  EXPECT_FALSE(bleturner::pollEvent(ev));
}

}  // namespace
