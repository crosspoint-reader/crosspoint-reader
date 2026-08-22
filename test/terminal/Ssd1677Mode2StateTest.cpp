#include <gtest/gtest.h>

#include "Ssd1677Mode2State.h"

namespace {

using freeink::Ssd1677Mode2State;

TEST(Ssd1677Mode2StateTest, UnavailableBuildAlwaysUsesLegacyPath) {
  Ssd1677Mode2State state;

  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::Legacy);
  state.seeded();
  state.enabled();
  state.presented();
  EXPECT_FALSE(state.active());
  EXPECT_EQ(state.activationCount(), 0U);
}

TEST(Ssd1677Mode2StateTest, RequiresAbsoluteSeedBeforeFirstPingPongUpdate) {
  Ssd1677Mode2State state;
  state.setAvailable(true);

  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::SeedAbsolute);
  state.seeded();
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::EnableAndUpdate);
  state.enabled();
  EXPECT_TRUE(state.active());
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::PingPongUpdate);
}

TEST(Ssd1677Mode2StateTest, TracksOddAndEvenFullAndWindowActivations) {
  Ssd1677Mode2State state;
  state.setAvailable(true);
  state.seeded();
  state.enabled();

  state.presented();  // full-frame target
  EXPECT_EQ(state.phase(), Ssd1677Mode2State::Phase::NeedsSync);
  EXPECT_EQ(state.activationCount(), 1U);
  EXPECT_EQ(state.bankParity(), 1);
  state.resynchronized();
  state.presented();  // byte-aligned window target
  EXPECT_EQ(state.activationCount(), 2U);
  EXPECT_EQ(state.bankParity(), 0);
  state.resynchronized();
  state.presented();  // disjoint window target
  EXPECT_EQ(state.activationCount(), 3U);
  EXPECT_EQ(state.bankParity(), 1);
  state.resynchronized();
  EXPECT_EQ(state.phase(), Ssd1677Mode2State::Phase::Active);
}

TEST(Ssd1677Mode2StateTest, UnsynchronizedBankCannotStartAnotherUpdate) {
  Ssd1677Mode2State state;
  state.setAvailable(true);
  state.seeded();
  state.enabled();
  state.presented();

  EXPECT_TRUE(state.active());
  EXPECT_EQ(state.phase(), Ssd1677Mode2State::Phase::NeedsSync);
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::ResetAndSeedAbsolute);
  state.resynchronized();
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::PingPongUpdate);
}

TEST(Ssd1677Mode2StateTest, IncompatibleCleanRestoresAndRequiresASeed) {
  Ssd1677Mode2State state;
  state.setAvailable(true);
  state.seeded();
  state.enabled();
  state.presented();
  state.resynchronized();

  EXPECT_EQ(state.plan(false), Ssd1677Mode2State::Plan::ResetAndSeedAbsolute);
  state.controllerReset();
  EXPECT_FALSE(state.active());
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::SeedAbsolute);
  state.seeded();
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::EnableAndUpdate);
}

TEST(Ssd1677Mode2StateTest, PowerLossAndAbortClearParityAndActivationCount) {
  Ssd1677Mode2State state;
  state.setAvailable(true);
  state.seeded();
  state.enabled();
  state.presented();
  state.resynchronized();
  state.presented();
  state.resynchronized();
  state.presented();

  state.controllerReset();
  EXPECT_EQ(state.phase(), Ssd1677Mode2State::Phase::NeedsSeed);
  EXPECT_EQ(state.activationCount(), 0U);
  EXPECT_EQ(state.bankParity(), 0);
}

TEST(Ssd1677Mode2StateTest, LegacyWriteInvalidatesAnUnactivatedSeed) {
  Ssd1677Mode2State state;
  state.setAvailable(true);
  state.seeded();
  EXPECT_EQ(state.plan(false), Ssd1677Mode2State::Plan::Legacy);

  state.invalidate();
  EXPECT_EQ(state.plan(true), Ssd1677Mode2State::Plan::SeedAbsolute);
}

}  // namespace
