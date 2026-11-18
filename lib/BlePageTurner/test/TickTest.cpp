// The main-loop pass: when a book visit starts the radio, when it retries, idles, rearms,
// holds for a chapter build, and how a remote press reaches the book.

#include <gtest/gtest.h>

#include <cstring>

#include "Fakes.h"
#include "Runtime.h"

namespace {

using bleturner::Action;
using bleturner::BuildRelease;
using bleturner::Config;
using bleturner::Scene;
using fake::host;
using fake::radio;

class TickTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    std::strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
    bleturner::begin(fake::hostFns(), config);
    // Starts run on their own task: queued, run by the test after the pass that asked.
    radio().queueStarts = true;
  }
  bool pass(const Scene& s) {
    const bool acted = bleturner::tick(s);
    if (radio().startQueued) radio().runStart();
    return acted;
  }
  // A running radio on a shown page, started the normal way.
  void running(const uint32_t visit = 1) {
    pass(fake::reading(visit));
    ASSERT_TRUE(radio().running);
  }
  void link(const char* name) {
    radio().connected = true;
    radio().addr = "7d:de:5c:bd:ae:ca";
    radio().name = name;
  }
  void edge(const uint32_t code, const bool pressed, const uint8_t keycode = 0) {
    bleturner::port::RawEdge e;
    e.value = code & 0xFF;
    e.byteIndex = (code >> 8) & 0xFF;
    e.reportId = (code >> 16) & 0xFF;
    e.pressed = pressed;
    e.keycode = keycode;
    e.atMs = radio().now;
    radio().raw.push_back(e);
  }
  bool logged(const char* text) const {
    for (const auto& line : host().logs) {
      if (line.find(text) != std::string::npos) return true;
    }
    return false;
  }
  Config config;
};

TEST_F(TickTest, HomeNeverStartsTheRadioInTheX3Profile) {
  std::strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  Scene s{};
  s.where = bleturner::Where::Home;
  s.visit = 1;
  s.pageShown = true;
  for (int i = 0; i < 3; ++i) pass(s);
  EXPECT_EQ(radio().creates, 0u);
  EXPECT_FALSE(bleturner::beforeScreenChange(0, /*nextIsReader=*/true) && radio().running);
}

TEST_F(TickTest, StartsOnceTheBookShowsItsPageAndOncePerVisit) {
  Scene s = fake::reading();
  s.pageShown = false;
  pass(s);
  EXPECT_EQ(radio().creates, 0u) << "Home and an unpainted page keep their memory";
  pass(fake::reading());
  EXPECT_EQ(host().yields, 1u) << "the book frees its layout right before the start";
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_TRUE(radio().running);
  EXPECT_TRUE(logged("Reader BLE start requested"));
  for (int i = 0; i < 5; ++i) pass(fake::reading());
  EXPECT_EQ(radio().creates, 1u);
  // The next visit finds it running: nothing to start, the book is not asked to yield again.
  pass(fake::reading(2));
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_EQ(host().yields, 1u);
}

TEST_F(TickTest, NoStartOutsideAShownBookOrWhileItIndexes) {
  Scene s = fake::reading();
  s.bookIndexing = true;
  pass(s);
  s = fake::reading();
  s.where = bleturner::Where::Elsewhere;
  pass(s);
  config.enabled = 0;
  pass(fake::reading(2));
  EXPECT_EQ(radio().creates, 0u);
  EXPECT_EQ(host().yields, 0u);
}

TEST_F(TickTest, BookNotReadyToYieldIsAskedAgainNextPass) {
  host().yieldResult = false;
  pass(fake::reading());
  EXPECT_EQ(radio().creates, 0u);
  host().yieldResult = true;
  pass(fake::reading());
  EXPECT_EQ(host().yields, 2u);
  EXPECT_EQ(radio().creates, 1u);
  radio().now += 6000;
  pass(fake::reading());
  EXPECT_EQ(host().yields, 2u) << "a running radio does not ask the book again";
}

TEST_F(TickTest, MemoryRefusalRetriesEveryFiveSecondsAtMostSixTimes) {
  host().heap = {65535, 32768};
  pass(fake::reading());
  ASSERT_EQ(radio().creates, 1u);
  EXPECT_TRUE(bleturner::status().readerDeferred);
  for (int i = 0; i < 49; ++i) {
    radio().now += 100;
    pass(fake::reading());
  }
  EXPECT_EQ(radio().creates, 1u) << "no retry before five seconds";
  for (int i = 0; i < 120; ++i) {
    radio().now += 1000;
    pass(fake::reading());
  }
  EXPECT_EQ(radio().creates, 7u) << "one start and six retries";
  EXPECT_TRUE(logged("Retrying reader BLE start after memory refusal (6)"));
}

TEST_F(TickTest, IdleThirtySecondsWithoutARemoteStopsAndAPageKeyLeavesItOff) {
  running();
  pass(fake::reading());  // the idle clock starts on the first pass that sees the radio up
  radio().now += 29999;
  pass(fake::reading());
  EXPECT_TRUE(radio().running);
  radio().now += 1;
  pass(fake::reading());
  EXPECT_FALSE(radio().running);
  EXPECT_TRUE(bleturner::status().idleStopped);
  EXPECT_EQ(bleturner::why(), bleturner::Why::IdleNoLink);
  EXPECT_TRUE(logged("Radio idle for 30000 ms with nothing connected; stopping"));
  for (int i = 0; i < 20; ++i) pass(fake::reading());
  EXPECT_EQ(radio().creates, 1u) << "idle ticks never restart it";
  Scene s = fake::reading();
  s.localKey = true;
  pass(s);
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_FALSE(radio().running);
  EXPECT_TRUE(bleturner::status().idleStopped);
}

TEST_F(TickTest, ALinkedRemoteNeverIdles) {
  running();
  link("Free3-R");
  radio().now += 600000;
  pass(fake::reading());
  EXPECT_TRUE(radio().running);
}

TEST_F(TickTest, TurnedOffOrCardTakenStopsTheRadio) {
  running();
  Scene s = fake::reading();
  s.storageBusy = true;
  pass(s);
  EXPECT_FALSE(radio().running);
  running(2);
  config.enabled = 0;
  pass(fake::reading(2));
  EXPECT_FALSE(radio().running);
}

TEST_F(TickTest, ReaderToggleRestartsWithoutChangingDefaultMappings) {
  config.enabled = 0;
  Scene scene = fake::reading();
  pass(scene);
  EXPECT_FALSE(radio().running);

  config.enabled = 1;
  scene.pageShown = false;
  pass(scene);
  EXPECT_FALSE(radio().running);
  scene.pageShown = true;
  pass(scene);
  ASSERT_TRUE(radio().running);

  config.enabled = 0;
  EXPECT_TRUE(bleturner::switchOff());
  pass(scene);
  EXPECT_FALSE(radio().running);
  config.enabled = 1;
  pass(scene);
  EXPECT_TRUE(radio().running);
  EXPECT_EQ(config.remoteCount, 0);
  EXPECT_EQ(bleturner::pageActionFor(config, bleturner::kUsageRight, 0), Action::NextPage);
  EXPECT_EQ(bleturner::pageActionFor(config, bleturner::kUsageLeft, 0), Action::PrevPage);
}

// Device (d2, 51033 ms): a page release restarted the radio a starved build had just stopped;
// the start then held the main loop for 2.85 s.
TEST_F(TickTest, BuildHoldWaitsForThePageNotAKey) {
  running();
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::Released);
  EXPECT_FALSE(radio().running);
  EXPECT_TRUE(logged("Section build starved of heap; stopping the radio until the page is shown"));
  Scene s = fake::reading();
  s.localKey = true;
  pass(s);
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::NotHeld) << "already stopped for this build";
  bleturner::afterPaint();
  pass(fake::reading());
  EXPECT_EQ(radio().creates, 2u);
  EXPECT_TRUE(radio().running);
}

TEST_F(TickTest, BuildHoldDoesNotOutliveTheVisit) {
  running();
  ASSERT_EQ(bleturner::beforeChapterBuild(), BuildRelease::Released);
  pass(fake::reading(2));
  Scene s = fake::reading(2);
  s.localKey = true;
  pass(s);
  EXPECT_EQ(radio().creates, 2u);
}

TEST_F(TickTest, BuildLeavesAnOffOrIdleRadioAlone) {
  config.enabled = 0;
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::NotHeld);
  config.enabled = 1;
  ASSERT_TRUE(bleturner::detail::stopForIdle());
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::NotHeld);
  EXPECT_EQ(radio().endCalls, 0u);
}

TEST_F(TickTest, BuildReportsARadioThatDoesNotStop) {
  running();
  radio().endResult = false;
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::StillUp);
  EXPECT_GE(radio().now - 10000, 3000u);
  EXPECT_TRUE(logged("ERR Radio did not stop for the section build"));
}

// A start past the render lock (inside the stack's own init) is waited for, then stopped.
TEST_F(TickTest, BuildWaitsForAStartPastTheLockThenStopsIt) {
  bleturner::tick(fake::reading());
  ASSERT_TRUE(radio().startQueued);
  radio().onSleep = [] {
    if (radio().startQueued) radio().runStart();
  };
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::Released);
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_EQ(radio().endCalls, 1u);
  EXPECT_FALSE(radio().running);
}

// The render task holds its lock through the build. A start still waiting for that lock
// gives up its turn: the build does not sit out 3 s for a start that cannot finish.
TEST_F(TickTest, BuildDoesNotWaitOnAStartThatNeedsTheRenderLock) {
  bleturner::tick(fake::reading());
  ASSERT_TRUE(radio().startQueued);
  host().renderBusy = true;
  radio().onSleep = [] {
    if (radio().startQueued) radio().runStart();
  };
  EXPECT_EQ(bleturner::beforeChapterBuild(), BuildRelease::Released);
  EXPECT_LT(radio().now - 10000, 100u);
  EXPECT_EQ(radio().beginCalls, 0u);
  EXPECT_EQ(host().releaseCalls, 0u);
  EXPECT_FALSE(bleturner::status().readerDeferred);
  EXPECT_FALSE(bleturner::holdsHeap());
}

// X3, 07/10/2026: the render task restored a page beside a stack coming up (free 22,676 after the
// stack) and ran the heap dry: abort(), since a nothrow new cannot even throw with no heap left. A
// paint lets a start in flight finish first, and leaves the radio it started up.
TEST_F(TickTest, PaintWaitsForAStartPastTheLock) {
  bleturner::tick(fake::reading());
  ASSERT_TRUE(radio().startQueued);
  radio().onSleep = [] {
    if (radio().startQueued) radio().runStart();
  };
  bleturner::settleStart();
  EXPECT_FALSE(bleturner::status().starting);
  EXPECT_EQ(radio().beginCalls, 1u);
  EXPECT_TRUE(radio().running);
}

TEST_F(TickTest, PaintDoesNotWaitOnAStartThatNeedsTheRenderLock) {
  bleturner::tick(fake::reading());
  ASSERT_TRUE(radio().startQueued);
  host().renderBusy = true;
  radio().onSleep = [] {
    if (radio().startQueued) radio().runStart();
  };
  bleturner::settleStart();
  EXPECT_FALSE(bleturner::status().starting);
  EXPECT_LT(radio().now - 10000, 100u);
  EXPECT_EQ(radio().beginCalls, 0u);
}

TEST_F(TickTest, PaintWithNoStartInFlightDoesNotWait) {
  running();
  bleturner::settleStart();
  EXPECT_EQ(radio().now, 10000u);
  EXPECT_TRUE(radio().running);
}

// A sheet over the page (the reader menu) stops the radio before it is drawn: a connecting stack
// and the sheet's page snapshot (52 KB on the X3) do not fit the heap together. While the sheet is
// up the book is not in front; back on the page the radio starts again.
TEST_F(TickTest, SheetStopsTheRadioFirstAndThePageStartsItAgain) {
  running();
  EXPECT_TRUE(bleturner::stopNow(1000));
  EXPECT_FALSE(radio().running);
  Scene sheet = fake::reading();
  sheet.where = bleturner::Where::Elsewhere;
  pass(sheet);
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_FALSE(radio().running);
  pass(fake::reading());
  EXPECT_EQ(radio().creates, 2u);
  EXPECT_TRUE(radio().running);
}

TEST_F(TickTest, SheetCancelsAStartInFlight) {
  bleturner::tick(fake::reading());
  ASSERT_TRUE(radio().startQueued);
  radio().onSleep = [] {
    if (radio().startQueued) radio().runStart();
  };
  EXPECT_TRUE(bleturner::stopNow(1000));
  EXPECT_FALSE(bleturner::status().starting);
  EXPECT_FALSE(radio().running);
  EXPECT_EQ(radio().beginCalls, 0u);
}

TEST_F(TickTest, BondedPriorityPolicyIsArmedOnceBeforeTheFirstPoll) {
  config.pick = 0;
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  running();
  pass(fake::reading());
  ASSERT_GE(radio().events.size(), 2u);
  EXPECT_EQ(radio().events[0], "arm");
  EXPECT_EQ(radio().events[1], "poll");
  EXPECT_EQ(radio().armedAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_EQ(radio().armedPick, 0);
  pass(fake::reading());
  EXPECT_EQ(radio().armCalls, 1u);
  pass(fake::reading(2));
  EXPECT_EQ(radio().armCalls, 2u) << "a new visit arms it again";
}

TEST_F(TickTest, FirstPolicyAndEmptyPriorityArePropagatedToRadio) {
  config.pick = 1;
  running();
  config.peerAddr[0] = '\0';  // a radio left up after the remote was forgotten
  pass(fake::reading());
  EXPECT_EQ(radio().armedPick, 1);
  EXPECT_EQ(radio().armedAddr, "");
  EXPECT_EQ(radio().armCalls, 1u);
}

TEST_F(TickTest, ConnectedRemoteTurnsPagesWithItsOwnTable) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  config.remoteCount = 2;
  strcpy(config.remotes[0].addr, config.peerAddr);
  config.remotes[0].count = 1;
  config.remotes[0].bindings[0] = bleturner::makeBinding(0x030102, false, Action::NextPage);
  strcpy(config.remotes[1].addr, "11:22:33:44:55:66");
  config.remotes[1].count = 1;
  config.remotes[1].bindings[0] = bleturner::makeBinding(0x030102, false, Action::PrevChapter);
  running();
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Other Remote";
  pass(fake::reading());
  edge(0x030102, true);
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::PrevChapter}));
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::None);
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
  radio().connected = false;
  pass(fake::reading());
  link("Priority Remote");
  pass(fake::reading());
  edge(0x030102, true);
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::PrevChapter, Action::NextPage}));
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::None);
}

TEST_F(TickTest, ConnectedNonPriorityRemoteWithoutATableUsesDecodedKeys) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  running();
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  pass(fake::reading());
  radio().keys = {{bleturner::kUsageRight, 0, true}};
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
}

// Remote A is chosen and bonded. Pairing remote B fails: A stays the chosen one, and when A links
// again in the book its presses turn pages.
TEST_F(TickTest, FailedPairingKeepsTheChosenRemote) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  strcpy(config.peerName, "Free3-R");
  EXPECT_TRUE(bleturner::pair("11:22:33:44:55:66", "Remote B"));
  EXPECT_EQ(radio().connects, (std::vector<std::string>{"11:22:33:44:55:66"}));
  EXPECT_FALSE(bleturner::service()) << "B never linked";
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_STREQ(config.peerName, "Free3-R");
  pass(fake::reading());
  link("Some Remote");  // A again
  pass(fake::reading());
  radio().keys = {{bleturner::kUsageRight, 0, true}};
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
}

TEST_F(TickTest, FailedConnectRequestDoesNotPromoteCandidate) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  strcpy(config.peerName, "Free3-R");
  radio().connecting = true;
  radio().connectResult = false;
  EXPECT_FALSE(bleturner::pair("11:22:33:44:55:66", "Remote B"));
  EXPECT_EQ(radio().connects, (std::vector<std::string>{"11:22:33:44:55:66"}));
  radio().connecting = false;
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Remote B";
  EXPECT_FALSE(bleturner::service());
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_STREQ(config.peerName, "Free3-R");
}

TEST_F(TickTest, FailedStartDoesNotPromoteCandidate) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  strcpy(config.peerName, "Free3-R");
  radio().beginResult = false;
  EXPECT_FALSE(bleturner::pair("11:22:33:44:55:66", "Remote B"));
  EXPECT_TRUE(radio().connects.empty());
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Remote B";
  EXPECT_FALSE(bleturner::service());
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_STREQ(config.peerName, "Free3-R");
}

TEST_F(TickTest, PairedRemoteBecomesTheChosenOneOnceItLinks) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_TRUE(bleturner::pair("11:22:33:44:55:66", "Remote B"));
  radio().connected = true;
  radio().addr = "7d:de:5c:bd:ae:ca";  // A linked meanwhile: not the remote being paired
  EXPECT_FALSE(bleturner::service());
  EXPECT_STREQ(config.peerAddr, "7d:de:5c:bd:ae:ca");
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Remote B";
  EXPECT_TRUE(bleturner::service()) << "the host saves the new choice";
  EXPECT_STREQ(config.peerAddr, "11:22:33:44:55:66");
  EXPECT_STREQ(config.peerName, "Remote B");
  EXPECT_FALSE(bleturner::service()) << "saved once";
}

// v1.0.50 saved the choice before a pairing completed. A choice left on a remote that never bonded
// cannot link again: the bonded remote that does link turns pages instead of being dropped.
TEST_F(TickTest, AChoiceThatIsNotBondedDoesNotBlockTheLinkedRemote) {
  strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  radio().bonds = {"11:22:33:44:55:66"};
  running();
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Remote A";
  pass(fake::reading());
  radio().keys = {{bleturner::kUsageRight, 0, true}};
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
}

TEST_F(TickTest, WithNoChosenRemoteAnyLinkedRemoteTurnsPages) {
  running();
  radio().connected = true;
  radio().addr = "11:22:33:44:55:66";
  radio().name = "Other Remote";
  pass(fake::reading());
  radio().keys = {{bleturner::kUsageRight, 0, true}};
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
}

TEST_F(TickTest, ThreeButtonRemoteTapSkipsAChapterAndItsHoldFrameGoesBack) {
  running();
  link("Free3-R");
  edge(0x030102, true);  // queued before the table was chosen: dropped
  pass(fake::reading());
  EXPECT_TRUE(host().delivered.empty());
  edge(0x030102, true);
  EXPECT_TRUE(pass(fake::reading()));
  edge(0x030102, false);
  edge(0x030008, true, 0x30);
  pass(fake::reading());
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextChapter, Action::PrevChapter}));
}

TEST_F(TickTest, KeyPathWithoutATableTurnsOnPressesOnly) {
  running();
  link("Some Remote");
  pass(fake::reading());
  radio().keys = {{bleturner::kUsageRight, 0, true},
                  {bleturner::kUsageRight, 0, false},
                  {bleturner::kUsagePageUp, 0, true},
                  {bleturner::kUsageRight, 0x02, true},
                  {0x04, 0, true}};
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage, Action::PrevPage}));
  EXPECT_TRUE(logged("key 0x4F mods 0x00 down -> next"));
  EXPECT_TRUE(logged("key 0x04 mods 0x00 down -> none"));
  host().deliverResult = false;
  radio().keys = {{bleturner::kUsageRight, 0, true}};
  EXPECT_FALSE(pass(fake::reading())) << "a turn the book refused is no user activity";
}

TEST_F(TickTest, WithATableTheKeyEventOfTheSameFrameIsOnlyLogged) {
  running();
  link("Free3-R");
  pass(fake::reading());
  config.nextKeyUsage = 0x02;
  edge(0x030002, true, 0x02);
  radio().keys = {{0x02, 0, true}};
  pass(fake::reading());
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
}

TEST_F(TickTest, HoldSlotTapActsOnReleaseHoldAtTheThreshold) {
  running();
  link("Some Remote");
  auto* t = bleturner::editableTable(config.remotes, config.remoteCount, "7d:de:5c:bd:ae:ca", "Some Remote");
  ASSERT_TRUE(t && bleturner::learn(*t, Action::NextPage, 0x030102, false) &&
              bleturner::learn(*t, Action::NextChapter, 0x030102, true));
  pass(fake::reading());
  edge(0x030102, true);
  pass(fake::reading());
  EXPECT_TRUE(host().delivered.empty());
  radio().now += 100;
  edge(0x030102, false);
  pass(fake::reading());
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::NextPage}));
  edge(0x030102, true);
  radio().now += bleturner::kHoldMs - 1;
  pass(fake::reading());
  EXPECT_EQ(host().delivered.size(), 1u);
  radio().now += 1;
  pass(fake::reading());
  EXPECT_EQ(host().delivered.back(), Action::NextChapter);
  edge(0x030102, false);
  pass(fake::reading());
  EXPECT_EQ(host().delivered.size(), 2u) << "the release after a hold does nothing";
}

TEST_F(TickTest, ShortcutsCountAsActivityWhateverTheBookSays) {
  running();
  link("Some Remote");
  auto* t = bleturner::editableTable(config.remotes, config.remoteCount, "7d:de:5c:bd:ae:ca", "Some Remote");
  ASSERT_TRUE(t && bleturner::learn(*t, Action::ReaderMenu, 0x030001, false));
  pass(fake::reading());
  host().deliverResult = false;
  edge(0x030001, true);
  EXPECT_TRUE(pass(fake::reading()));
  EXPECT_EQ(host().delivered, (std::vector<Action>{Action::ReaderMenu}));
}

TEST_F(TickTest, HeapRestartOnlyFromAQuietShownPage) {
  host().heap = {87308, 23540};
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(bleturner::switchOn());
  ASSERT_TRUE(bleturner::detail::heapRestartWanted());
  bleturner::tick(fake::reading());
  EXPECT_EQ(host().restarts, 0u) << "not while the book's own start is in flight";
  radio().runStart();
  Scene s = fake::reading();
  s.sleeping = true;
  pass(s);
  s = fake::reading();
  s.wifiOn = true;
  pass(s);
  s = fake::reading();
  s.pageShown = false;
  pass(s);
  EXPECT_EQ(host().restarts, 0u);
  pass(fake::reading());
  EXPECT_EQ(host().restarts, 1u);
  EXPECT_TRUE(logged("Heap fragmented for radio: free=87308 largest=23540; silent restart to reader"));
  pass(fake::reading());
  EXPECT_EQ(host().restarts, 1u);
}

// The book start rolls back on its heap three times (the first try and two 5 s retries), then
// the book restarts into itself from its shown page.
TEST_F(TickTest, BookWhoseStartsRollBackInPiecesRestartsIntoItself) {
  host().heap = {83228, 61428};
  radio().changeHeapOnBegin = true;
  radio().heapAfterBegin = {28812, 26612};
  for (int i = 0; i < 40 && host().restarts == 0; ++i) {
    radio().now += 1000;
    pass(fake::reading());
  }
  EXPECT_EQ(radio().beginCalls, 3u);
  EXPECT_EQ(host().restarts, 1u);
  EXPECT_TRUE(logged("Heap fragmented for radio: free=83228 largest=61428; silent restart to reader"));
}

TEST_F(TickTest, HoldsHeapWhileTheRadioOwnsOrAsksForIt) {
  EXPECT_FALSE(bleturner::holdsHeap());
  bleturner::tick(fake::reading());
  EXPECT_TRUE(bleturner::holdsHeap()) << "a start in flight";
  radio().runStart();
  EXPECT_TRUE(bleturner::holdsHeap()) << "running";
  config.enabled = 0;
  EXPECT_FALSE(bleturner::holdsHeap()) << "switched off in settings: the book stops deferring at once";
}

}  // namespace
