#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "AutoSleepSyncPolicy.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                SavedProgressPosition localKoPos, std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt,
                                KOReaderSyncRunMode runMode = KOReaderSyncRunMode::MANUAL,
                                SleepCommitCallback sleepCommitCallback = nullptr, AutoSleepSyncDeadline deadline = {})
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)),
        runMode(runMode),
        sleepCommitCallback(sleepCommitCallback),
        deadline(deadline) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  // Sleep preflight is terminal: a home gesture must not replace this activity,
  // or the coordinator would wait forever for a commit that can no longer come.
  bool handleHomeGesture() override { return isSleepMode(); }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;
  KOReaderSyncRunMode runMode;
  SleepCommitCallback sleepCommitCallback;
  AutoSleepSyncDeadline deadline;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool smartSyncEnabled() const;
  bool isSleepMode() const { return runMode == KOReaderSyncRunMode::SLEEP; }
  bool startStage(KOReaderSyncStage stage);
  bool routeTerminal(KOReaderSyncTerminalReason reason, KOReaderSyncTerminalAction manualAction);
  uint32_t elapsedMs() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
};
