#include "AutoSleepSyncPolicy.h"

#include <algorithm>

AutoSleepSyncRequestAction AutoSleepSyncCoordinator::request(const AutoSleepSyncContext requestedContext,
                                                             const bool eligible) {
  if (currentState != AutoSleepSyncState::IDLE) return AutoSleepSyncRequestAction::IGNORE;

  latchedContext = requestedContext;
  currentState = eligible ? AutoSleepSyncState::PREFLIGHT : AutoSleepSyncState::COMMITTED;
  return eligible ? AutoSleepSyncRequestAction::PREPARE : AutoSleepSyncRequestAction::COMMIT;
}

AutoSleepSyncSnapshotAction AutoSleepSyncCoordinator::finishSnapshot(const bool snapshotSucceeded) {
  if (currentState != AutoSleepSyncState::PREFLIGHT) return AutoSleepSyncSnapshotAction::COMMIT_SLEEP;

  snapshotStored = latchedContext.quickResume && snapshotSucceeded;
  if (latchedContext.quickResume && !snapshotStored) {
    currentState = AutoSleepSyncState::COMMITTED;
    return AutoSleepSyncSnapshotAction::COMMIT_SLEEP;
  }
  return AutoSleepSyncSnapshotAction::CONTINUE_PREFLIGHT;
}

AutoSleepSyncSnapshotCleanupAction AutoSleepSyncCoordinator::snapshotCleanupAction() const {
  return snapshotStored ? AutoSleepSyncSnapshotCleanupAction::RESTORE_AND_REMOVE
                        : AutoSleepSyncSnapshotCleanupAction::REMOVE_ONLY;
}

AutoSleepSyncPreflightAction AutoSleepSyncCoordinator::finishPreparation(const bool prepared,
                                                                         const bool activityAllocated) {
  if (currentState == AutoSleepSyncState::PREFLIGHT && prepared && activityAllocated) {
    return AutoSleepSyncPreflightAction::SCHEDULE_SYNC;
  }

  currentState = AutoSleepSyncState::COMMITTED;
  return AutoSleepSyncPreflightAction::COMMIT;
}

void AutoSleepSyncCoordinator::requestCommit() { currentState = AutoSleepSyncState::COMMITTED; }

bool AutoSleepSyncCoordinator::claimCommit() {
  if (currentState != AutoSleepSyncState::COMMITTED || commitClaimed) return false;
  commitClaimed = true;
  return true;
}

AutoSleepSyncDeadline AutoSleepSyncDeadline::fromNow(const uint32_t nowMs, const uint32_t budgetMs) {
  const uint32_t boundedBudgetMs = budgetMs <= MAX_BUDGET_MS ? budgetMs : MAX_BUDGET_MS;
  return AutoSleepSyncDeadline(nowMs + boundedBudgetMs);
}

uint32_t AutoSleepSyncDeadline::remainingMs(const uint32_t nowMs) const {
  const int32_t remaining = static_cast<int32_t>(deadlineMs - nowMs);
  return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
}

AutoSleepSyncPreference AutoSleepSyncPolicy::normalizePreference(const int32_t rawPreference) {
  return rawPreference == static_cast<uint8_t>(AutoSleepSyncPreference::WHEN_SLEEPING)
             ? AutoSleepSyncPreference::WHEN_SLEEPING
             : AutoSleepSyncPreference::OFF;
}

bool AutoSleepSyncPolicy::shouldPersistPreference(const AutoSleepSyncPreference current,
                                                  const AutoSleepSyncPreference requested) {
  return current != normalizePreference(static_cast<uint8_t>(requested));
}

bool AutoSleepSyncPolicy::isEligible(const AutoSleepSyncPreference preference, const bool smartSyncEnabled,
                                     const bool hasCredentials, const bool fromReader) {
  return preference == AutoSleepSyncPreference::WHEN_SLEEPING && smartSyncEnabled && hasCredentials && fromReader;
}

bool AutoSleepSyncPolicy::shouldSkipForPosition(const AutoSleepSyncMarkerData& marker, const uint32_t serverFingerprint,
                                                const int spineIndex, const int pageNumber, const int totalPages) {
  if (marker.serverFingerprint != serverFingerprint) return false;
  // Out-of-range values cannot have been recorded in the uint16 marker fields.
  if (spineIndex < 0 || pageNumber < 0 || totalPages < 0 || spineIndex > UINT16_MAX || pageNumber > UINT16_MAX ||
      totalPages > UINT16_MAX) {
    return false;
  }
  // Spine indexes are stable across relayouts (one spine per chapter), so an
  // earlier chapter is unambiguously behind.
  if (spineIndex < marker.spineIndex) return true;
  if (spineIndex > marker.spineIndex) return false;
  // Same spine: page numbers are only comparable under the same pagination.
  // A different page count means render settings changed; sync to be safe.
  if (marker.totalPages != static_cast<uint16_t>(totalPages)) return false;
  return static_cast<uint16_t>(pageNumber) <= marker.pageNumber;
}

KOReaderSyncTerminalAction AutoSleepSyncPolicy::terminalAction(const KOReaderSyncRunMode mode,
                                                               const KOReaderSyncTerminalAction manualAction) {
  return mode == KOReaderSyncRunMode::SLEEP ? KOReaderSyncTerminalAction::COMMIT_SLEEP : manualAction;
}

bool AutoSleepSyncPolicy::shouldStartStage(const KOReaderSyncRunMode mode, const AutoSleepSyncDeadline deadline,
                                           const uint32_t nowMs) {
  return mode == KOReaderSyncRunMode::MANUAL || !deadline.expired(nowMs);
}

WifiSelectionExhaustedAction AutoSleepSyncPolicy::wifiExhaustedAction(const WifiSelectionMode mode) {
  return mode == WifiSelectionMode::HEADLESS ? WifiSelectionExhaustedAction::COMPLETE_FAILURE
                                             : WifiSelectionExhaustedAction::SHOW_NETWORK_LIST;
}

bool AutoSleepSyncPolicy::shouldStartWifiScan(const WifiSelectionMode mode, const bool hasSavedCredentials,
                                              const bool scanAlreadyStarted, const AutoSleepSyncDeadline deadline,
                                              const uint32_t nowMs) {
  if (mode == WifiSelectionMode::MANUAL) return true;
  return hasSavedCredentials && !scanAlreadyStarted && !deadline.expired(nowMs);
}

uint32_t AutoSleepSyncPolicy::clampWifiStageMs(const AutoSleepSyncDeadline deadline, const uint32_t nowMs,
                                               const uint32_t stageLimitMs) {
  return std::min(deadline.remainingMs(nowMs), stageLimitMs);
}
