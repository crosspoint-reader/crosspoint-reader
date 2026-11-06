// S3 build (FREEINK_BLE_STACK_IN_PSRAM): the radio may start at Home once the first frame is up, with a
// remote chosen, and stays up into the book. Heap numbers are the ones measured on the unit.

#include <gtest/gtest.h>

#include <cstring>

#include "Fakes.h"
#include "Runtime.h"

namespace {

using bleturner::Config;
using bleturner::Scene;
using bleturner::Where;
using fake::host;
using fake::radio;

class HomeStartTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    std::strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
    bleturner::begin(fake::hostFns(), config);
    radio().queueStarts = true;
    host().heap = {124752, 81908};
    radio().changeHeapOnBegin = true;
    radio().heapAfterBegin = {78244, 36852};
  }
  static Scene home(const uint32_t visit = 1, const bool shown = true) {
    Scene s{};
    s.where = Where::Home;
    s.visit = visit;
    s.pageShown = shown;
    return s;
  }
  bool pass(const Scene& s) {
    const bool acted = bleturner::tick(s);
    if (radio().startQueued) radio().runStart();
    return acted;
  }
  Config config;
};

TEST_F(HomeStartTest, StartsAfterTheFirstFrameAndOncePerVisit) {
  pass(home(1, false));
  EXPECT_EQ(radio().creates, 0u) << "Home paints its own frame first";
  pass(home());
  EXPECT_EQ(radio().creates, 1u);
  EXPECT_TRUE(radio().running);
  for (int i = 0; i < 5; ++i) pass(home());
  EXPECT_EQ(radio().creates, 1u);
}

TEST_F(HomeStartTest, NeedsTheRemoteOnAndChosen) {
  config.peerAddr[0] = '\0';
  pass(home());
  EXPECT_EQ(radio().creates, 0u);
  std::strcpy(config.peerAddr, "7d:de:5c:bd:ae:ca");
  config.enabled = 0;
  pass(home(2));
  EXPECT_EQ(radio().creates, 0u);
}

TEST_F(HomeStartTest, ARefusalAtHomeNeverAsksForARestartIntoTheBook) {
  host().heap = {124752, 60000};
  for (uint32_t visit = 1; visit <= 6; ++visit) pass(home(visit));
  EXPECT_EQ(radio().beginCalls, 0u) << "the stack is never brought up for a heap Home cannot spare";
  EXPECT_FALSE(radio().running);
  EXPECT_EQ(host().restarts, 0u);
}

TEST_F(HomeStartTest, IsKeptWhenTheNextScreenIsTheBookAndStoppedForAnyOther) {
  pass(home());
  ASSERT_TRUE(radio().running);
  EXPECT_TRUE(bleturner::beforeScreenChange(0, /*nextIsReader=*/true));
  EXPECT_TRUE(radio().running);
  EXPECT_TRUE(bleturner::beforeScreenChange(0, /*nextIsReader=*/false));
  EXPECT_FALSE(radio().running);
}

TEST_F(HomeStartTest, TheBookFindsTheRadioUpAndDoesNotStartItAgain) {
  pass(home());
  ASSERT_TRUE(bleturner::beforeScreenChange(0, true));
  const unsigned before = radio().creates;
  pass(fake::reading(2));
  EXPECT_EQ(radio().creates, before);
  EXPECT_TRUE(radio().running);
}

TEST_F(HomeStartTest, PressesMadeAtHomeAreDroppedNotCarriedIntoTheBook) {
  pass(home());
  ASSERT_TRUE(radio().running);
  bleturner::port::RawEdge e;
  e.value = 0x02;
  e.byteIndex = 1;
  e.reportId = 3;
  e.pressed = true;
  e.atMs = radio().now;
  radio().raw.push_back(e);
  bleturner::port::KeyPress k;
  k.keycode = bleturner::kUsageRight;
  k.pressed = true;
  radio().keys.push_back(k);
  pass(home());
  EXPECT_TRUE(radio().raw.empty());
  EXPECT_TRUE(radio().keys.empty());
  EXPECT_TRUE(host().delivered.empty());
}

TEST_F(HomeStartTest, PressQueuedBetweenHomeAndReaderCannotTurnTheBook) {
  pass(home());
  radio().connected = true;
  radio().name = "Generic Remote";
  ASSERT_TRUE(bleturner::beforeScreenChange(0, true));
  bleturner::port::KeyPress key;
  key.keycode = bleturner::kUsageRight;
  key.pressed = true;
  radio().keys.push_back(key);
  pass(fake::reading(2));
  EXPECT_TRUE(radio().keys.empty());
  EXPECT_TRUE(host().delivered.empty());

  radio().keys.push_back(key);
  pass(fake::reading(2));
  ASSERT_EQ(host().delivered.size(), 1u);
  EXPECT_EQ(host().delivered.front(), bleturner::Action::NextPage);
}

// The radio that comes up at Home is there for the remote too: without the reconnect armed and polled it
// finds nothing to link, and 30 s later the idle rule stops it for good (a60a, 14/10).
TEST_F(HomeStartTest, HomeArmsTheBondedReconnectOnceAndPollsIt) {
  pass(home());
  ASSERT_TRUE(radio().running);
  for (int i = 0; i < 10; ++i) {
    radio().now += 1000;
    pass(home());
  }
  EXPECT_EQ(radio().armCalls, 1u);
  EXPECT_GE(radio().polls, 10u);
  ASSERT_FALSE(radio().events.empty());
  EXPECT_EQ(radio().events.front(), "arm");
  EXPECT_EQ(radio().armedAddr, "7d:de:5c:bd:ae:ca");
  // A new Home visit arms again (the SDK treats a repeat of the same arm as a no-op).
  pass(home(2));
  EXPECT_EQ(radio().armCalls, 2u);
}

TEST_F(HomeStartTest, ARemoteLinkedAtHomeKeepsTheRadioUpPastTheIdleLimit) {
  pass(home());
  ASSERT_TRUE(radio().running);
  radio().connected = true;
  for (int i = 0; i < 90; ++i) {
    radio().now += 1000;
    pass(home());
  }
  EXPECT_TRUE(radio().running);
  EXPECT_TRUE(host().delivered.empty());
}

// The 30 s a remote gets to link count from the visit it is asked for: a book opened 25 s after the radio came up
// at Home has its own 30 s, as a book that starts the radio itself has (a60a, 14/10).
TEST_F(HomeStartTest, TheIdleLimitStartsOverWhenTheBookOpens) {
  pass(home());
  ASSERT_TRUE(radio().running);
  for (int i = 0; i < 25; ++i) {
    radio().now += 1000;
    pass(home());
  }
  ASSERT_TRUE(radio().running);
  ASSERT_TRUE(bleturner::beforeScreenChange(0, /*nextIsReader=*/true));
  int stoppedAfter = -1;
  for (int s = 1; s <= 40 && stoppedAfter < 0; ++s) {
    radio().now += 1000;
    pass(fake::reading(2));
    if (!radio().running) stoppedAfter = s;
  }
  EXPECT_GE(stoppedAfter, 30) << "the book got " << stoppedAfter << " s to link";
}

// 13/10 11:38-12:10 on the unit: the first pairing can only start from Settings > Bluetooth page turner, where the
// screens hold about 18 KB. The beta came up with 70,840 / 31,732 left and was rolled back by the X3 block rule.
TEST_F(HomeStartTest, TheFirstPairingStartsFromTheSettingsScreen) {
  host().heap = {117360, 73716};
  radio().heapAfterBegin = {70840, 31732};
  EXPECT_TRUE(bleturner::switchOn());
  EXPECT_TRUE(radio().running);
  EXPECT_EQ(radio().endCalls, 0u) << "not rolled back";
  for (const auto& line : host().logs) EXPECT_EQ(line.find("rolled back"), std::string::npos) << line;
}

}  // namespace
