#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

// Where a "child" activity (launched via one of the replaceWith* helpers) should route
// control when it exits successfully. See ActivityManager::returnFromChild().
enum class ReturnTo : uint8_t { Home, FileBrowser, RecentBooks };

// Minimal state the returning parent needs to restore its previous view (directory,
// focused item, list index). Kept as a plain struct stored by value on the
// ActivityManager — single instance, overwritten per transition, no heap churn
// beyond the two small std::strings.
struct ReturnHint {
  ReturnTo target = ReturnTo::Home;
  std::string path;        // FileBrowser directory to restore
  std::string selectName;  // item to re-focus in a list (file name, book title)
  int selectIndex = -1;    // e.g. Recents index
};

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
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  volatile bool requestedUpdate = false;

  // When true, input events are consumed (discarded) until all buttons are released
  // and no press/release events remain.  Armed automatically on activity transitions
  // (push / pop / replace) so that the button used to leave one activity cannot bleed
  // into the next one.
  bool drainInput = false;

  // Where returnFromChild() should route to. Set by replaceWith*() helpers, cleared
  // in returnFromChild(). Cleared on any manual goHome()/goTo*() to avoid stale hints
  // outliving the flow they were recorded for.
  ReturnHint returnHint;
  bool hasReturnHint = false;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings();
  void goToFileBrowser(std::string path = {}, std::string focusName = {});
  void goToRecentBooks(int focusIndex = -1);
  void goToGlobalBookmarks();
  void goToBrowser();
  void goToReader(std::string path);
  void goToKOReaderSync();
  void goToSleep();
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToWeather();
  void goHome(std::string focusBookPath = {}, int focusSelectorIndex = -1);

  // Replace-with-hint helpers: destroy the current activity before launching the new
  // one (freeing its memory) and record where to route control when the new activity
  // exits. Consumed by returnFromChild().
  void replaceWithReader(std::string path, ReturnHint hint);
  void replaceWithFileBrowser(std::string path, ReturnHint hint, std::string focusName = {});
  void replaceWithRecentBooks(ReturnHint hint);

  // Called by a "child" activity on successful exit. Consults the stored ReturnHint,
  // clears it, and dispatches to the corresponding parent with restoration args. If
  // no hint is set, defaults to goHome().
  void returnFromChild();

  // Record a ReturnHint before calling any plain goTo*() helper. Allows an activity
  // (e.g. Home) to declare "when this flow ends, come back here with this state" for
  // transitions where we don't want a dedicated replaceWith*() wrapper.
  // Cleared by returnFromChild() or by an explicit goHome()/replaceWith*() call.
  void setReturnHint(ReturnHint hint) {
    returnHint = std::move(hint);
    hasReturnHint = true;
  }
  void clearReturnHint() {
    returnHint = {};
    hasReturnHint = false;
  }

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  bool skipLoopDelay() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
