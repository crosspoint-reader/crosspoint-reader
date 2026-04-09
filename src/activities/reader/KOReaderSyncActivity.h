#pragma once
#include <Epub.h>

#include <functional>
#include <memory>

#include "ChapterXPathIndexer.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Shared pipeline:
 * 1. Connect to WiFi (if not connected)
 * 2. Optionally sync NTP (if stale)
 * 3. Calculate document hash
 *
 * Intent-specific behavior:
 * - COMPARE: fetch remote progress, show full comparison screen, let user
 *   choose Apply or Upload.
 * - PULL_REMOTE: fetch and map remote progress, show success feedback, then
 *   return applied SyncResult to reader.
 * - PUSH_LOCAL: compute local mapping, warm session with GET, then upload via
 *   reused connection to avoid a second full TLS handshake.
 */
class KOReaderSyncActivity final : public Activity {
 public:
  // Intent controls UI/behavior split for the same sync pipeline.
  // - COMPARE: fetch then let user choose apply/upload.
  // - PULL_REMOTE: fetch and apply immediately.
  // - PUSH_LOCAL: upload immediately.
  // This keeps WiFi/NTP/hash/memory handling centralized while enabling a
  // simpler KOReader-like reader menu UX.
  enum class SyncIntent {
    COMPARE,
    PULL_REMOTE,
    PUSH_LOCAL,
  };

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::shared_ptr<Epub>& epub, const std::string& epubPath, int currentSpineIndex,
                                int currentPage, int totalPagesInSpine, uint16_t paragraphIndex = 0,
                                bool hasParagraphIndex = false, SyncIntent syncIntent = SyncIntent::COMPARE)
      : Activity("KOReaderSync", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        localParagraphIndex(paragraphIndex),
        hasLocalParagraphIndex(hasParagraphIndex),
        syncIntent(syncIntent),
        remoteProgress{},
        remotePosition{},
        localProgress{} {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    APPLY_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  uint16_t localParagraphIndex;
  bool hasLocalParagraphIndex;
  SyncIntent syncIntent = SyncIntent::COMPARE;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  bool remotePositionMapped = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (for display)
  KOReaderPosition localProgress;
  std::string remoteChapterLabel;
  std::string localChapterLabel;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timestamp when completion state was entered (for auto-close)
  unsigned long uploadCompleteTime = 0;
  bool closeRequested = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  void closeCancelled();
  bool ensureEpubLoadedForMapping();
  void releaseEpubForMapping();
  bool computeLocalProgressAndChapter();
  void computeRemoteChapter();
  bool ensureRemotePositionMapped(bool closeSessionBeforeMapping = true);
};
