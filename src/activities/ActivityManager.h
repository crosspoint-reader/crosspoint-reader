#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "settings/SettingsSection.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class HomeMenuItem { NONE, FILE_BROWSER, LIBRARY, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

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
  enum class PendingAction { None, Push, Pop, Replace, ReplaceForSleep };
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
  std::atomic<bool> requestedUpdate{false};

  // Counts screen changes, so the Bluetooth page turner can tell one visit of a book from the next.
  // Written by the main loop under RenderLock; the render task captures it under that lock.
  uint32_t screenVisit = 0;
  // The screen visit whose frame the render task painted last.
  std::atomic<uint32_t> paintedVisit{0};

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~ActivityManager();

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToJoinNetwork();  // File Transfer straight into Join Network (post heap-defrag reboot)
  void goToUsbDrive();
  // openSection jumps straight into one group (End = the section list).
  void goToSettings(SettingsSection openSection = SettingsSection::End);
  void goToFileBrowser(std::string path = {});
  void goToLibrary();
  void goToBrowser();
  void goToPlugins(bool showOpds);
  void goToReader(std::string path, bool allowFastInitialRefresh = false);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool requiresExclusiveStorageLoop() const;
  bool isReaderActivity() const;
  // Bluetooth page turner (lib/BlePageTurner): the book in front, whether it has painted a page, and
  // a page or chapter turn from a remote. False when no book in front took the turn.
  bool isForegroundReader() const;
  bool isRemoteReaderContext() const;
  bool remoteClick(RemoteButton button);
  // Home is in front and has painted a frame of its own (not one left from the screen before).
  bool isForegroundHomeShown() const;
  uint32_t visit() const { return screenVisit; }
  bool remoteTurn(bool forward, bool chapter);
  // A remote button mapped to the reader menu (true) or to a new clipping (false).
  bool remoteShortcut(bool readerMenu);
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;
  void prepareForSleep();

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
