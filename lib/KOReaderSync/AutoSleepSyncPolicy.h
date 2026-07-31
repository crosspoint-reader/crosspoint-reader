#pragma once

#include <cstdint>

enum class AutoSleepSyncPreference : uint8_t {
  OFF = 0,
  WHEN_SLEEPING = 1,
};

enum class KOReaderSyncRunMode : uint8_t {
  MANUAL = 0,
  SLEEP = 1,
};

enum class KOReaderSyncTerminalAction : uint8_t {
  RETURN_TO_READER = 0,
  SHOW_RESULT = 1,
  COMMIT_SLEEP = 2,
};

enum class KOReaderSyncTerminalReason : uint8_t {
  WIFI_FAILED = 0,
  ACTIVITY_OOM,
  NO_CREDENTIALS,
  HASH_FAILED,
  GET_FAILED,
  AUTH_FAILED,
  SERVER_FAILED,
  JSON_FAILED,
  LOW_MEMORY,
  EPUB_LOAD_FAILED,
  SAVE_FAILED,
  UPLOAD_FAILED,
  UPLOAD_COMPLETE,
  ALREADY_SYNCED,
  REMOTE_APPLIED,
  DEADLINE_EXPIRED,
};

enum class KOReaderSyncStage : uint8_t {
  NTP = 0,
  PRIMARY_HASH,
  PRIMARY_GET,
  ALTERNATE_HASH,
  ALTERNATE_GET,
  EPUB_RELOAD,
  REMOTE_MAPPING,
  PUT,
};

enum class WifiSelectionMode : uint8_t {
  MANUAL = 0,
  HEADLESS = 1,
};

enum class WifiSelectionExhaustedAction : uint8_t {
  SHOW_NETWORK_LIST = 0,
  COMPLETE_FAILURE = 1,
};

enum class AutoSleepSyncState : uint8_t {
  IDLE = 0,
  PREFLIGHT = 1,
  COMMITTED = 2,
};

enum class AutoSleepSyncRequestAction : uint8_t {
  IGNORE = 0,
  PREPARE = 1,
  COMMIT = 2,
};

enum class AutoSleepSyncPreflightAction : uint8_t {
  SCHEDULE_SYNC = 0,
  COMMIT = 1,
};

enum class AutoSleepSyncSnapshotAction : uint8_t {
  CONTINUE_PREFLIGHT = 0,
  COMMIT_SLEEP = 1,
};

enum class AutoSleepSyncSnapshotCleanupAction : uint8_t {
  REMOVE_ONLY = 0,
  RESTORE_AND_REMOVE = 1,
};

class AutoSleepSyncDeadline final {
 public:
  static constexpr uint32_t DEFAULT_BUDGET_MS = 25000;
  static constexpr uint32_t MAX_BUDGET_MS = INT32_MAX;

  constexpr AutoSleepSyncDeadline() = default;
  static AutoSleepSyncDeadline fromNow(uint32_t nowMs, uint32_t budgetMs = DEFAULT_BUDGET_MS);

  uint32_t deadlineAtMs() const { return deadlineMs; }
  uint32_t remainingMs(uint32_t nowMs) const;
  bool expired(uint32_t nowMs) const { return remainingMs(nowMs) == 0; }

 private:
  explicit constexpr AutoSleepSyncDeadline(uint32_t absoluteDeadlineMs) : deadlineMs(absoluteDeadlineMs) {}

  uint32_t deadlineMs = 0;
};

// Last successfully synced position for one book, persisted per book on SD so
// an eligible sleep can skip the whole preflight when nothing moved since the
// last sync against the same server identity.
struct AutoSleepSyncMarkerData {
  uint32_t serverFingerprint = 0;  // hash of server URL + username + match method
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t totalPages = 0;  // pagination guard: a relayout invalidates the marker
};

struct AutoSleepSyncContext {
  bool fromReader = false;
  bool fromTimeout = false;
  bool quickResume = false;
  AutoSleepSyncDeadline deadline{};
};

class AutoSleepSyncCoordinator final {
 public:
  AutoSleepSyncRequestAction request(AutoSleepSyncContext requestedContext, bool eligible);
  AutoSleepSyncSnapshotAction finishSnapshot(bool snapshotSucceeded);
  AutoSleepSyncPreflightAction finishPreparation(bool prepared, bool activityAllocated);
  void requestCommit();
  bool claimCommit();

  AutoSleepSyncState state() const { return currentState; }
  const AutoSleepSyncContext& context() const { return latchedContext; }
  AutoSleepSyncSnapshotCleanupAction snapshotCleanupAction() const;

 private:
  AutoSleepSyncState currentState = AutoSleepSyncState::IDLE;
  AutoSleepSyncContext latchedContext{};
  bool snapshotStored = false;
  bool commitClaimed = false;
};

class AutoSleepSyncPolicy final {
 public:
  static AutoSleepSyncPreference normalizePreference(int32_t rawPreference);
  static bool shouldPersistPreference(AutoSleepSyncPreference current, AutoSleepSyncPreference requested);
  static bool isEligible(AutoSleepSyncPreference preference, bool smartSyncEnabled, bool hasCredentials,
                         bool fromReader);
  static bool positionUnchanged(const AutoSleepSyncMarkerData& marker, uint32_t serverFingerprint, int spineIndex,
                                int pageNumber, int totalPages);
  static KOReaderSyncTerminalAction terminalAction(KOReaderSyncRunMode mode, KOReaderSyncTerminalAction manualAction);
  static bool shouldStartStage(KOReaderSyncRunMode mode, AutoSleepSyncDeadline deadline, uint32_t nowMs);
  static WifiSelectionExhaustedAction wifiExhaustedAction(WifiSelectionMode mode);
  static bool shouldStartWifiScan(WifiSelectionMode mode, bool hasSavedCredentials, bool scanAlreadyStarted,
                                  AutoSleepSyncDeadline deadline, uint32_t nowMs);
  static uint32_t clampWifiStageMs(AutoSleepSyncDeadline deadline, uint32_t nowMs, uint32_t stageLimitMs);
};
