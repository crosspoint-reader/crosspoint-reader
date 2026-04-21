#pragma once
#include <Epub.h>

#include <memory>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Syncs reading progress with a Readest-compatible KOReader sync server.
 *
 * Flow: WiFi → hash document → fetch remote → show comparison → apply or upload.
 */
class ReadestSyncActivity final : public Activity {
 public:
  explicit ReadestSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::shared_ptr<Epub>& epub, const std::string& epubPath, int currentSpineIndex,
                               int currentPage, int totalPagesInSpine)
      : Activity("ReadestSync", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        remoteProgress{},
        remotePosition{},
        localProgress{} {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }
  bool isReaderActivity() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;
  KOReaderPosition localProgress;

  int selectedOption = 0;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
};
