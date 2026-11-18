#include <gtest/gtest.h>

#include <cstring>

#include "Fakes.h"
#include "Runtime.h"

namespace {
class RequestConnectTest : public ::testing::Test {
 protected:
  bleturner::Config config;
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    config.pick = 1;
    std::strcpy(config.peerAddr, "11:22:33:44:55:66");
    bleturner::begin(fake::hostFns(), config);
    fake::radio().queueStarts = true;
  }
  void pass() {
    bleturner::tick(fake::reading());
    if (fake::radio().startQueued) fake::radio().runStart();
  }
};

TEST_F(RequestConnectTest, T2IdleStopRearmsOnceIncludingRepeatedHoldsDuringStart) {
  pass();
  pass();
  fake::radio().now += 30000;
  pass();
  ASSERT_TRUE(bleturner::status().idleStopped);
  const unsigned starts = fake::radio().creates;
  bleturner::requestConnect();
  EXPECT_TRUE(bleturner::status().idleStopped);
  bleturner::tick(fake::reading());
  EXPECT_FALSE(bleturner::status().idleStopped);
  EXPECT_EQ(fake::radio().creates, starts + 1);
  bleturner::requestConnect();
  bleturner::tick(fake::reading());
  EXPECT_EQ(fake::radio().creates, starts + 1);
  fake::radio().runStart();
  pass();
  EXPECT_EQ(fake::radio().armedPick, config.pick);
  EXPECT_EQ(fake::radio().armedAddr, config.peerAddr);
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::Connecting);
}

TEST_F(RequestConnectTest, T3LinkedRemoteKeepsLinkAndReportsItsName) {
  pass();
  pass();
  fake::radio().connected = true;
  fake::radio().addr = "7d:de:5c:bd:ae:ca";
  fake::radio().name = "Remote A";
  pass();
  const unsigned starts = fake::radio().creates;
  const unsigned stops = fake::radio().endCalls;
  const unsigned arms = fake::radio().armCalls;
  bleturner::requestConnect();
  pass();
  EXPECT_EQ(fake::radio().creates, starts);
  EXPECT_EQ(fake::radio().endCalls, stops);
  EXPECT_EQ(fake::radio().armCalls, arms);
  EXPECT_TRUE(fake::radio().connects.empty());
  EXPECT_TRUE(fake::radio().connected);
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::Connected);
  EXPECT_STREQ(bleturner::linked().name, "Remote A");
  bleturner::acknowledgeLinkNote();
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::None);
}

TEST_F(RequestConnectTest, T4LowTotalIsRefusedWithNoteAndNoRestart) {
  fake::host().heap = {81919, 32768};
  bleturner::requestConnect();
  pass();
  pass();
  EXPECT_EQ(fake::radio().beginCalls, 0u);
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::LowMemory);
  for (unsigned repeat = 0; repeat < 8; ++repeat) {
    fake::radio().now += 5000;
    pass();
  }
  EXPECT_EQ(fake::host().restarts, 0u);
}

TEST_F(RequestConnectTest, T4FragmentedIsRefusedWithNoteAndNoRestart) {
  fake::host().heap = {81920, 32767};
  bleturner::requestConnect();
  pass();
  pass();
  EXPECT_EQ(fake::radio().beginCalls, 0u);
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::LowMemory);
  for (unsigned repeat = 0; repeat < 8; ++repeat) {
    fake::radio().now += 5000;
    pass();
  }
  EXPECT_EQ(fake::host().restarts, 0u);
}

TEST_F(RequestConnectTest, T4ExactHeapBoundaryStartsAndArmsSavedPick) {
  fake::host().heap = {81920, 32768};
  bleturner::requestConnect();
  EXPECT_EQ(fake::radio().creates, 0u);
  pass();
  pass();
  EXPECT_EQ(fake::radio().creates, 1u);
  EXPECT_EQ(fake::radio().beginCalls, 1u);
  EXPECT_EQ(fake::radio().armCalls, 1u);
  EXPECT_EQ(fake::radio().armedPick, config.pick);
  EXPECT_EQ(fake::radio().armedAddr, config.peerAddr);
}

TEST_F(RequestConnectTest, LinkedAtFirstTickStillReportsConnected) {
  fake::radio().running = true;
  fake::radio().connected = true;
  bleturner::requestConnect();
  pass();
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::Connected);
  EXPECT_EQ(fake::radio().creates, 0u);
}

TEST_F(RequestConnectTest, PostInitFragmentationNeverRestartsAfterManualRequest) {
  fake::radio().changeHeapOnBegin = true;
  fake::radio().heapAfterBegin = {28812, 26612};
  bleturner::requestConnect();
  pass();
  pass();
  EXPECT_EQ(bleturner::linkNote(), bleturner::LinkNote::LowMemory);
  EXPECT_FALSE(fake::radio().running);
  EXPECT_EQ(fake::host().restarts, 0u);
}
}  // namespace
