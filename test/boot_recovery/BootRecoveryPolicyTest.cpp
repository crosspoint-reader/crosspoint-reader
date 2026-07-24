#include <gtest/gtest.h>

#include "BootRecoveryPolicy.h"

TEST(BootRecoveryPolicy, ColdBootIgnoresUntrustedRtcContents) {
  BootRecoveryRecord record;
  record.magic = BootRecoveryPolicy::MAGIC;
  record.currentStage = 0xff;

  const auto decision = BootRecoveryPolicy::begin(record, false, false);

  EXPECT_FALSE(decision.active);
  EXPECT_TRUE(BootRecoveryPolicy::valid(record));
  EXPECT_EQ(record.skipMask, 0);
}

TEST(BootRecoveryPolicy, CrashSkipsOnlyTheStageThatWasActive) {
  BootRecoveryRecord record;
  BootRecoveryPolicy::begin(record, false, false);
  BootRecoveryPolicy::enterStage(record, BootStage::RecentBooks);

  const auto decision = BootRecoveryPolicy::begin(record, true, false);

  EXPECT_TRUE(decision.active);
  EXPECT_EQ(decision.failedStage, BootStage::RecentBooks);
  EXPECT_TRUE(BootRecoveryPolicy::shouldSkip(decision.skipMask, BootStage::RecentBooks));
  EXPECT_FALSE(BootRecoveryPolicy::shouldSkip(decision.skipMask, BootStage::Settings));
}

TEST(BootRecoveryPolicy, RepeatedCrashAccumulatesOnlyObservedFailingStages) {
  BootRecoveryRecord record;
  BootRecoveryPolicy::begin(record, false, false);
  BootRecoveryPolicy::enterStage(record, BootStage::Settings);
  auto decision = BootRecoveryPolicy::begin(record, true, false);
  EXPECT_TRUE(BootRecoveryPolicy::shouldSkip(decision.skipMask, BootStage::Settings));

  BootRecoveryPolicy::enterStage(record, BootStage::SdFonts);
  decision = BootRecoveryPolicy::begin(record, true, false);
  EXPECT_TRUE(BootRecoveryPolicy::shouldSkip(decision.skipMask, BootStage::Settings));
  EXPECT_TRUE(BootRecoveryPolicy::shouldSkip(decision.skipMask, BootStage::SdFonts));
}

TEST(BootRecoveryPolicy, NonCrashResetDoesNotTreatInterruptedStageAsFailure) {
  BootRecoveryRecord record;
  BootRecoveryPolicy::begin(record, false, false);
  BootRecoveryPolicy::enterStage(record, BootStage::Opds);

  const auto decision = BootRecoveryPolicy::begin(record, false, false);

  EXPECT_FALSE(decision.active);
  EXPECT_EQ(decision.skipMask, 0);
}

TEST(BootRecoveryPolicy, ManualSafeStartupSkipsEveryOptionalStage) {
  BootRecoveryRecord record;
  const auto decision = BootRecoveryPolicy::begin(record, false, true);

  EXPECT_TRUE(decision.active);
  EXPECT_TRUE(decision.manual);
  EXPECT_EQ(decision.skipMask, BootRecoveryPolicy::allStageMask());
  for (uint8_t stage = static_cast<uint8_t>(BootStage::Settings);
       stage <= static_cast<uint8_t>(BootStage::SdFonts); ++stage) {
    EXPECT_TRUE(BootRecoveryPolicy::shouldSkip(decision.skipMask, static_cast<BootStage>(stage)));
  }
}

TEST(BootRecoveryPolicy, LeavingAStagePreventsFalseRecoveryOnLaterCrash) {
  BootRecoveryRecord record;
  BootRecoveryPolicy::begin(record, false, false);
  BootRecoveryPolicy::enterStage(record, BootStage::KOReader);
  BootRecoveryPolicy::leaveStage(record, BootStage::KOReader);

  const auto decision = BootRecoveryPolicy::begin(record, true, false);

  EXPECT_FALSE(decision.active);
  EXPECT_EQ(decision.failedStage, BootStage::None);
}
