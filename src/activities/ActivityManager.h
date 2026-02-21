#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <string>
#include <vector>

#include "Activity.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"

struct Intent {
  // Note: only include trivial copiable data here, do NOT pass a pointer or reference
  std::string path;

  // FullScreenMessage
  std::string message;
  EpdFontFamily::Style messageStyle;
};

// TODO: move this to the correct place
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

struct ActivityResult {
  // Note: only include trivial copiable data here, do NOT pass a pointer or reference
  bool isCancelled = false;

  // For NetworkModeSelectionActivity result
  NetworkMode selectedNetworkMode = NetworkMode::JOIN_NETWORK;

  // For WifiSelectionActivity result
  std::string wifiSSID;
  std::string wifiIP;
  bool wifiConnected = false;

  // For EpubReaderMenuActivity result
  uint8_t selectedOrientation = 0;

  // For KeyboardEntryActivity result
  std::string inputText;

  // For XtcReaderChapterSelectionActivity result
  uint32_t selectedPage = 0;

  // For KOReaderSyncActivity result
  int syncedSpineIndex = 0;
  int syncedPage = 0;

  // For EpubReaderMenuActivity result (-1 = back/cancelled, else cast of MenuAction)
  int menuAction = -1;

  // For EpubReaderChapterSelectionActivity result
  int selectedSpineIndex = 0;

  // For EpubReaderPercentSelectionActivity result
  int selectedPercent = 0;
};

class Activity;  // forward declaration

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 */
class ActivityManager {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<Activity*> stackActivities;
  Activity* currentActivity = nullptr;

  void exitActivity();

  // Pending activity to be launched on next loop iteration
  Activity* pendingActivity = nullptr;
  enum PendingAction { Push, Pop, Replace };
  PendingAction pendingAction = Replace;
  ActivityResult pendingResult;  // for Pop with result

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(Activity* newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings();
  void goToMyLibrary(Intent&& intent);
  void goToRecentBooks();
  void goToBrowser();
  void goToReader(Intent&& intent);
  void goToSleep();
  void goToBoot();
  void goToFullScreenMessage(Intent&& intent);
  void goHome();

  // This will move current activity to stack instead of deleting it
  void pushActivity(Activity* activity);
  void pushActivityForResult(Activity* activity, std::function<void(ActivityResult&)> resultHandler);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();
  void popActivityWithResult(ActivityResult& result);

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  bool skipLoopDelay() const;

  void requestUpdate();
};

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
