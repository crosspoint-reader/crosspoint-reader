#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload/Cancel)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public ActivityWithSubactivity {
 public:
  using OnCancelCallback = std::function<void()>;
  using OnSyncCompleteCallback = std::function<void(int newSpineIndex, int newPageNumber)>;

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::shared_ptr<Epub>& epub, const std::string& epubPath, int currentSpineIndex,
                                int currentPage, int totalPagesInSpine, OnCancelCallback onCancel,
                                OnSyncCompleteCallback onSyncComplete)
      : ActivityWithSubactivity("KOReaderSync", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        remoteProgress{},
        remotePosition{},
        localProgress{},
        onCancel(std::move(onCancel)),
        onSyncComplete(std::move(onSyncComplete)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

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

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (for display)
  KOReaderPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload, 2=Cancel)
  int selectedOption = 0;

  OnCancelCallback onCancel;
  OnSyncCompleteCallback onSyncComplete;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
};
