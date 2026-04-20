#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "BookFusionSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Syncs reading progress with BookFusion.
 *
 * Only works for books that have a BookFusion book_id sidecar
 * (i.e., books downloaded from BookFusion). Shows NOT_A_BF_BOOK otherwise.
 *
 * Flow:
 *  1. Verify token + book_id exist (early-out if not)
 *  2. Connect WiFi
 *  3. Fetch remote position
 *  4. Show comparison + options (Apply / Upload)
 *  5. Apply or upload
 */
class BookFusionSyncActivity final : public Activity {
 public:
  explicit BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::shared_ptr<Epub>& epub, const std::string& epubPath, int currentSpineIndex,
                                  int currentPage, int totalPagesInSpine)
      : Activity("BookFusionSync", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

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
    NO_TOKEN,
    NOT_A_BF_BOOK,
  };

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  uint32_t bookId = 0;

  // Remote progress
  BookFusionPosition remotePosition;
  CrossPointPosition remoteCrossPoint{};

  // Local progress in BF format (for display)
  BookFusionPosition localPosition;

  bool hasRemoteProgress = false;
  int selectedOption = 0;  // 0 = Apply remote, 1 = Upload local

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
};
