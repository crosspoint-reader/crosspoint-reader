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
#include "RenderLock.h"

struct Intent {
  // IMPORTANT: only include copiable data here, do NOT pass a pointer or reference

  // For MyLibraryActivity and ReaderActivity
  std::string path;

  // For FullScreenMessageActivity
  std::string message;
  EpdFontFamily::Style messageStyle = EpdFontFamily::REGULAR;
};

class Activity;    // forward declaration
class RenderLock;  // forward declaration

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No concept of Bundle or Intent extras
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<Activity*> stackActivities;
  Activity* currentActivity = nullptr;

  void exitActivity(RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  Activity* pendingActivity = nullptr;
  enum PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = None;

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

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  bool skipLoopDelay() const;

  void requestUpdate();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
