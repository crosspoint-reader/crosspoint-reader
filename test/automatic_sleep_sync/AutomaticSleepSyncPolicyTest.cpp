#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "AutoSleepSyncPolicy.h"
#include "src/activities/NearestEligibleActivity.h"

namespace {

TEST(AutoSleepSyncPreference, DefaultsAndInvalidValuesNormalizeToOff) {
  const AutoSleepSyncPreference defaultPreference{};
  EXPECT_EQ(defaultPreference, AutoSleepSyncPreference::OFF);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(0), AutoSleepSyncPreference::OFF);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(1), AutoSleepSyncPreference::WHEN_SLEEPING);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(-1), AutoSleepSyncPreference::OFF);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(2), AutoSleepSyncPreference::OFF);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(257), AutoSleepSyncPreference::OFF);
  EXPECT_EQ(AutoSleepSyncPolicy::normalizePreference(UINT8_MAX), AutoSleepSyncPreference::OFF);
}

TEST(AutoSleepSyncPreference, PersistsOnlyWhenNormalizedValueChanges) {
  EXPECT_FALSE(
      AutoSleepSyncPolicy::shouldPersistPreference(AutoSleepSyncPreference::OFF, AutoSleepSyncPreference::OFF));
  EXPECT_TRUE(AutoSleepSyncPolicy::shouldPersistPreference(AutoSleepSyncPreference::OFF,
                                                           AutoSleepSyncPreference::WHEN_SLEEPING));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldPersistPreference(AutoSleepSyncPreference::WHEN_SLEEPING,
                                                            AutoSleepSyncPreference::WHEN_SLEEPING));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldPersistPreference(AutoSleepSyncPreference::OFF,
                                                            static_cast<AutoSleepSyncPreference>(UINT8_MAX)));
}

TEST(AutoSleepSyncPolicy, EligibleOnlyForEnabledSmartCredentialedReaderSleep) {
  EXPECT_TRUE(AutoSleepSyncPolicy::isEligible(AutoSleepSyncPreference::WHEN_SLEEPING, true, true, true));

  EXPECT_FALSE(AutoSleepSyncPolicy::isEligible(AutoSleepSyncPreference::OFF, true, true, true));
  EXPECT_FALSE(AutoSleepSyncPolicy::isEligible(AutoSleepSyncPreference::WHEN_SLEEPING, false, true, true));
  EXPECT_FALSE(AutoSleepSyncPolicy::isEligible(AutoSleepSyncPreference::WHEN_SLEEPING, true, false, true));
  EXPECT_FALSE(AutoSleepSyncPolicy::isEligible(AutoSleepSyncPreference::WHEN_SLEEPING, true, true, false));
  EXPECT_FALSE(AutoSleepSyncPolicy::isEligible(static_cast<AutoSleepSyncPreference>(UINT8_MAX), true, true, true));
}

TEST(AutoSleepSyncDeadline, UsesSingleTwentyFiveSecondAbsoluteDeadline) {
  constexpr uint32_t startMs = 500;
  const auto deadline = AutoSleepSyncDeadline::fromNow(startMs);

  EXPECT_EQ(deadline.deadlineAtMs(), startMs + AutoSleepSyncDeadline::DEFAULT_BUDGET_MS);
  EXPECT_EQ(deadline.remainingMs(startMs), AutoSleepSyncDeadline::DEFAULT_BUDGET_MS);
  EXPECT_EQ(deadline.remainingMs(startMs + AutoSleepSyncDeadline::DEFAULT_BUDGET_MS - 1), 1u);
  EXPECT_EQ(deadline.remainingMs(startMs + AutoSleepSyncDeadline::DEFAULT_BUDGET_MS), 0u);
  EXPECT_TRUE(deadline.expired(startMs + AutoSleepSyncDeadline::DEFAULT_BUDGET_MS));
}

TEST(AutoSleepSyncDeadline, RemainingTimeIsWrapSafe) {
  constexpr uint32_t startMs = UINT32_MAX - 15;
  constexpr uint32_t budgetMs = 32;
  const auto deadline = AutoSleepSyncDeadline::fromNow(startMs, budgetMs);

  EXPECT_EQ(deadline.deadlineAtMs(), 16u);
  EXPECT_EQ(deadline.remainingMs(startMs), budgetMs);
  EXPECT_EQ(deadline.remainingMs(UINT32_MAX), 17u);
  EXPECT_EQ(deadline.remainingMs(0), 16u);
  EXPECT_EQ(deadline.remainingMs(15), 1u);
  EXPECT_EQ(deadline.remainingMs(16), 0u);
  EXPECT_EQ(deadline.remainingMs(17), 0u);
}

TEST(AutoSleepWifiPolicy, ManualMayShowNetworkListWhileHeadlessCompletesFailure) {
  EXPECT_EQ(AutoSleepSyncPolicy::wifiExhaustedAction(WifiSelectionMode::MANUAL),
            WifiSelectionExhaustedAction::SHOW_NETWORK_LIST);
  EXPECT_EQ(AutoSleepSyncPolicy::wifiExhaustedAction(WifiSelectionMode::HEADLESS),
            WifiSelectionExhaustedAction::COMPLETE_FAILURE);
}

TEST(AutoSleepWifiPolicy, HeadlessStartsAtMostOneSavedNetworkScanBeforeDeadline) {
  constexpr uint32_t startMs = 100;
  const auto deadline = AutoSleepSyncDeadline::fromNow(startMs, 500);

  EXPECT_TRUE(AutoSleepSyncPolicy::shouldStartWifiScan(WifiSelectionMode::HEADLESS, true, false, deadline, startMs));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldStartWifiScan(WifiSelectionMode::HEADLESS, false, false, deadline, startMs));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldStartWifiScan(WifiSelectionMode::HEADLESS, true, true, deadline, startMs));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldStartWifiScan(WifiSelectionMode::HEADLESS, true, false, deadline, 600));

  EXPECT_TRUE(AutoSleepSyncPolicy::shouldStartWifiScan(WifiSelectionMode::MANUAL, false, true, deadline, 600));
}

TEST(AutoSleepWifiPolicy, StageWorkIsClampedToAbsoluteDeadline) {
  constexpr uint32_t startMs = UINT32_MAX - 15;
  const auto deadline = AutoSleepSyncDeadline::fromNow(startMs, 32);

  EXPECT_EQ(AutoSleepSyncPolicy::clampWifiStageMs(deadline, startMs, 100), 32u);
  EXPECT_EQ(AutoSleepSyncPolicy::clampWifiStageMs(deadline, UINT32_MAX, 100), 17u);
  EXPECT_EQ(AutoSleepSyncPolicy::clampWifiStageMs(deadline, 10, 5), 5u);
  EXPECT_EQ(AutoSleepSyncPolicy::clampWifiStageMs(deadline, 15, 100), 1u);
  EXPECT_EQ(AutoSleepSyncPolicy::clampWifiStageMs(deadline, 16, 100), 0u);
}

TEST(AutoSleepSyncPolicy, ManualTerminalRoutingRemainsUnchanged) {
  constexpr std::array manualActions = {KOReaderSyncTerminalAction::RETURN_TO_READER,
                                        KOReaderSyncTerminalAction::SHOW_RESULT,
                                        KOReaderSyncTerminalAction::COMMIT_SLEEP};

  for (const auto action : manualActions) {
    EXPECT_EQ(AutoSleepSyncPolicy::terminalAction(KOReaderSyncRunMode::MANUAL, action), action);
  }
}

TEST(AutoSleepSyncPolicy, EverySleepTerminalRouteCommitsSleep) {
  constexpr std::array manualActions = {KOReaderSyncTerminalAction::RETURN_TO_READER,
                                        KOReaderSyncTerminalAction::SHOW_RESULT,
                                        KOReaderSyncTerminalAction::COMMIT_SLEEP};

  for (const auto action : manualActions) {
    EXPECT_EQ(AutoSleepSyncPolicy::terminalAction(KOReaderSyncRunMode::SLEEP, action),
              KOReaderSyncTerminalAction::COMMIT_SLEEP);
  }
}

TEST(AutoSleepSyncPolicy, SleepDoesNotStartPostWifiWorkAfterDeadline) {
  constexpr uint32_t startMs = 400;
  const auto deadline = AutoSleepSyncDeadline::fromNow(startMs, 10);

  EXPECT_TRUE(AutoSleepSyncPolicy::shouldStartStage(KOReaderSyncRunMode::SLEEP, deadline, startMs + 9));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldStartStage(KOReaderSyncRunMode::SLEEP, deadline, startMs + 10));
  EXPECT_TRUE(AutoSleepSyncPolicy::shouldStartStage(KOReaderSyncRunMode::MANUAL, deadline, startMs + 10));
}

TEST(AutoSleepSyncCoordinator, FirstRequestWinsAndPreservesFlags) {
  AutoSleepSyncCoordinator coordinator;

  EXPECT_EQ(coordinator.request({true, false, true}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_EQ(coordinator.request({false, true, false}, false), AutoSleepSyncRequestAction::IGNORE);
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::PREFLIGHT);
  EXPECT_TRUE(coordinator.context().fromReader);
  EXPECT_FALSE(coordinator.context().fromTimeout);
  EXPECT_TRUE(coordinator.context().quickResume);
}

TEST(AutoSleepSyncCoordinator, ManualAndTimeoutRequestsRetainDistinctOrigins) {
  AutoSleepSyncCoordinator manual;
  AutoSleepSyncCoordinator timeout;

  EXPECT_EQ(manual.request({true, false, false}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_EQ(timeout.request({true, true, false}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_FALSE(manual.context().fromTimeout);
  EXPECT_TRUE(timeout.context().fromTimeout);
}

TEST(AutoSleepSyncCoordinator, IneligibleAndPreparationFailuresCommit) {
  AutoSleepSyncCoordinator ineligible;
  EXPECT_EQ(ineligible.request({false, false, false}, false), AutoSleepSyncRequestAction::COMMIT);
  EXPECT_EQ(ineligible.state(), AutoSleepSyncState::COMMITTED);

  AutoSleepSyncCoordinator saveFailure;
  ASSERT_EQ(saveFailure.request({true, false, false}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_EQ(saveFailure.finishPreparation(false, true), AutoSleepSyncPreflightAction::COMMIT);
  EXPECT_EQ(saveFailure.state(), AutoSleepSyncState::COMMITTED);

  AutoSleepSyncCoordinator allocationFailure;
  ASSERT_EQ(allocationFailure.request({true, false, false}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_EQ(allocationFailure.finishPreparation(true, false), AutoSleepSyncPreflightAction::COMMIT);
  EXPECT_EQ(allocationFailure.state(), AutoSleepSyncState::COMMITTED);
}

TEST(AutoSleepSyncCoordinator, SuccessfulPreparationSchedulesAndCommitIsIdempotent) {
  AutoSleepSyncCoordinator coordinator;
  ASSERT_EQ(coordinator.request({true, true, true}, true), AutoSleepSyncRequestAction::PREPARE);
  EXPECT_EQ(coordinator.finishPreparation(true, true), AutoSleepSyncPreflightAction::SCHEDULE_SYNC);
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::PREFLIGHT);

  coordinator.requestCommit();
  EXPECT_TRUE(coordinator.claimCommit());
  EXPECT_FALSE(coordinator.claimCommit());
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::COMMITTED);
}

TEST(AutoSleepSyncCoordinator, QuickResumeSnapshotFailureCommitsBeforePreflight) {
  AutoSleepSyncCoordinator coordinator;
  ASSERT_EQ(coordinator.request({true, false, true}, true), AutoSleepSyncRequestAction::PREPARE);

  EXPECT_EQ(coordinator.finishSnapshot(false), AutoSleepSyncSnapshotAction::COMMIT_SLEEP);
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::COMMITTED);
  EXPECT_EQ(coordinator.snapshotCleanupAction(), AutoSleepSyncSnapshotCleanupAction::REMOVE_ONLY);
}

TEST(AutoSleepSyncCoordinator, QuickResumeSnapshotRestoresAndRemovesAtTerminalCommit) {
  AutoSleepSyncCoordinator coordinator;
  ASSERT_EQ(coordinator.request({true, true, true}, true), AutoSleepSyncRequestAction::PREPARE);

  EXPECT_EQ(coordinator.finishSnapshot(true), AutoSleepSyncSnapshotAction::CONTINUE_PREFLIGHT);
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::PREFLIGHT);
  EXPECT_EQ(coordinator.snapshotCleanupAction(), AutoSleepSyncSnapshotCleanupAction::RESTORE_AND_REMOVE);
}

TEST(AutoSleepSyncCoordinator, NonQuickResumePreflightNeedsNoSnapshotButStillCleansStaleFile) {
  AutoSleepSyncCoordinator coordinator;
  ASSERT_EQ(coordinator.request({true, false, false}, true), AutoSleepSyncRequestAction::PREPARE);

  EXPECT_EQ(coordinator.finishSnapshot(false), AutoSleepSyncSnapshotAction::CONTINUE_PREFLIGHT);
  EXPECT_EQ(coordinator.state(), AutoSleepSyncState::PREFLIGHT);
  EXPECT_EQ(coordinator.snapshotCleanupAction(), AutoSleepSyncSnapshotCleanupAction::REMOVE_ONLY);
}

TEST(AutoSleepSyncMarkerPolicy, SyncsOnlyWhenStrictlyAheadOfMarker) {
  AutoSleepSyncMarkerData marker;
  marker.serverFingerprint = 0xABCD1234u;
  marker.spineIndex = 7;
  marker.pageNumber = 42;
  marker.totalPages = 180;

  // Equal position: nothing to sync.
  EXPECT_TRUE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 7, 42, 180));
  // Behind within the same spine (rereading): skip, or the Smart pull would
  // jump the reader forward to the remote high-water mark.
  EXPECT_TRUE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 7, 10, 180));
  // Behind in an earlier spine: skip regardless of that spine's page count.
  EXPECT_TRUE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 6, 90, 95));
  // Strictly ahead: sync.
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 7, 43, 180));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 8, 0, 60));
  // Same spine but repaginated: pages are incomparable, sync to be safe.
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xABCD1234u, 7, 42, 181));
  // Different server identity invalidates the marker entirely.
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 0xDEAD0000u, 7, 10, 180));
}

TEST(AutoSleepSyncMarkerPolicy, OutOfRangePositionsNeverSkip) {
  AutoSleepSyncMarkerData marker;
  marker.serverFingerprint = 1;
  marker.spineIndex = 0;
  marker.pageNumber = 0;
  marker.totalPages = 0;

  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 1, -1, 0, 0));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 1, 0, -1, 0));
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 1, 0, 0, UINT16_MAX + 1));
  // A truncated int must not alias into a uint16 marker value.
  EXPECT_FALSE(AutoSleepSyncPolicy::shouldSkipForPosition(marker, 1, 0, 65536, 0));
}

TEST(AutoSleepSyncTraversal, SelectsCurrentThenNearestEligibleStackedOwner) {
  struct Candidate {
    int id;
    bool eligible;
  };
  Candidate bottomReader{1, true};
  Candidate nearerReader{2, true};
  Candidate topMenu{3, false};
  Candidate currentReader{4, true};
  const std::array<Candidate*, 3> stack = {&bottomReader, &nearerReader, &topMenu};
  const auto eligible = [](const Candidate& candidate) { return candidate.eligible; };

  EXPECT_EQ(findNearestEligibleActivity(&currentReader, stack, eligible), &currentReader);
  EXPECT_EQ(findNearestEligibleActivity(static_cast<Candidate*>(nullptr), stack, eligible), &nearerReader);
}

}  // namespace
