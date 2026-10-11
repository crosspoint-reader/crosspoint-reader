#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ButtonEventManager.h"
#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "TouchUi.h"
#include "activities/browser/OpdsBackTo.h"
#include "activities/home/LibraryTab.h"
#include "activities/network/NetworkMode.h"

class Activity;                       // forward declaration
class RenderLock;                     // forward declaration
enum class HomeMenuAction : uint8_t;  // activities/home/HomeMenu.h

// Where a "child" activity (launched via one of the replaceWith* helpers) should route
// control when it exits successfully. See ActivityManager::returnFromChild().
// Library carries the tab in ReturnHint::selectIndex, and an open author's hash in selectionContext.
// AllFiles is the file browser opened from Settings on every file of the card: it comes back as
// itself, so the images and other files it lists do not vanish on the way back from the viewer.
enum class ReturnTo : uint8_t { Home, Library, AllFiles, GlobalBookmarks };

// Minimal state the returning parent needs to restore its previous view (directory,
// focused item, list index, or bookmark selection). Kept as a plain struct stored by
// value on the ActivityManager — single instance, overwritten per transition, no heap
// churn beyond the small strings.
struct ReturnHint {
  ReturnTo target = ReturnTo::Home;
  std::string path;              // FileBrowser directory to restore
  std::string selectName;        // item to re-focus in a list (file name, book title)
  int selectIndex = -1;          // e.g. the Home carousel's index
  std::string selectionContext;  // optional activity-specific restore key
  int selectBookmarkIndex = -1;  // optional bookmark index for GlobalBookmarks
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
  ButtonEventManager& buttonEvents;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  bool sleepTransition = false;

  std::atomic<bool> activityUsesWifi{false};
  void refreshWifiActivityFlag();

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

  // True while the render task is inside currentActivity->render(). Set and cleared by the
  // render task WHILE HOLDING renderingMutex, which is what makes it usable as a lifetime
  // guard: a task that holds the mutex and reads this false knows no pass can start until it
  // releases, because starting one requires the same mutex.
  //
  // It exists because holding renderingMutex is not sufficient to prove the render task is
  // done with the activity — render() drops the mutex mid-pass (see the comment on
  // RenderLock(ExclusiveActivityAccess)) and then keeps using the object. Destroying the
  // activity in that window is a use-after-free, so every transition that can destroy
  // currentActivity acquires its lock through that constructor.
  std::atomic<bool> renderPassActive{false};

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  volatile bool requestedUpdate = false;

  // Set when something earlier in THIS loop() tick has already put the displayed frame in the
  // write framebuffer, so a later partial paint must not sync over it. Cleared at the top of
  // every tick. See the "Re-syncing the write buffer" pitfall in docs/activity-manager.md.
  bool framebufferPreparedThisTick = false;

  // Paints the "working on it" banner before a queued transition runs. See the .cpp.
  void showBusyIndicator();

  // When true, input events are consumed (discarded) until all buttons are released
  // and no press/release events remain.  Armed automatically on activity transitions
  // (push / pop / replace) so that the button used to leave one activity cannot bleed
  // into the next one.
  bool drainInput = false;

  // Where returnFromChild() should route to. Set by replaceWith*() helpers and
  // preserved across plain goTo*() chains so a chained navigation flow can still
  // restore its original parent state. Cleared only by returnFromChild() or by
  // explicit goHome()/replaceWith*() calls, not by ordinary goTo*() transitions.
  // Relevant symbols: ReturnHint, returnHint, hasReturnHint, returnFromChild(),
  // goHome(), goTo*(), replaceWith*().
  ReturnHint returnHint;
  bool hasReturnHint = false;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), buttonEvents(globalButtonEvents()) {
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  // File Transfer, its menu opening on `selected`'s row.
  void goToFileTransfer(NetworkMode selected = NetworkMode::JOIN_NETWORK);
  void goToSerialTransfer();  // direct entry (also used by the boot-into-transfer path)
  void goToUsbDrive();        // no-op logging an error on boards without FREEINK_CAP_USB_MSC
  void goToSettings();
  void goToClockSettings();
  void goToKOReaderSettings();
  // The Library on `tab`: Books in `path` (the root when empty); `focusName` selected; on Authors,
  // `author` (a hash) open.
  void goToLibrary(LibraryTab tab, std::string path = {}, std::string focusName = {},
                   std::optional<uint32_t> author = std::nullopt);
  void goToGlobalBookmarks();
  void goToGlobalBookmarks(ReturnHint hint);
  // The OPDS catalog, or the server picker first when more than one server is configured. Opened
  // from File Transfer, so Back returns there.
  void goToBrowser();
  // The same, searching `query` on arrival: a finished book's "more by this author". Back goes Home.
  void goToBrowserWithSearch(std::string query);
  void goToReader(std::string path);
  void goToKOReaderSync();
  // fromTimeout=true marks this as an auto-sleep (used to gate Quick Resume on Timeout).
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToWeather();
  void goToReadingStats();
  void goToHomeMore();
  // Opens a home screen entry (see HomeMenu.h). A plain goTo*(), so the caller's ReturnHint survives.
  void goToHomeMenuAction(HomeMenuAction action);
  void goHome(std::string focusBookPath = {}, int focusSelectorIndex = -1);

  // Replace-with-hint helpers: destroy the current activity before launching the new
  // one (freeing its memory) and record where to route control when the new activity
  // exits. Consumed by returnFromChild().
  void replaceWithReader(std::string path, ReturnHint hint);
  void replaceWithFileBrowser(std::string path, ReturnHint hint, std::string focusName = {});

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

  // Asks every activity buried on the stack to drop what it built only to draw itself
  // (Activity::releaseBuriedState()), for a download that needs the heap in one piece. A pop that
  // would resume one that dropped something reboots to Settings instead; the activities above it,
  // the caller's own children included, pop as usual. Returns how many dropped something.
  int releaseBuriedActivityState();

  // An activity on the stack, current or buried under a child, holds the radio.
  bool wifiActivityOnStack() const { return activityUsesWifi.load(std::memory_order_relaxed); }

  bool inSleepTransition() const { return sleepTransition; }

  bool preventAutoSleep() const;
  bool keepAwake() const;
  // True while the current activity owns the raw SD card (USB Drive). main.cpp
  // consults this to suspend its own loop work — see Activity::requiresExclusiveStorageLoop().
  bool requiresExclusiveStorageLoop() const;
  // True while a reader is anywhere on the stack -- including under its own menu or TOC.
  bool isReaderActivity() const;
  // True only while the reader itself is on top: the one case dispatchButtonAction() delivers a
  // configured action. A list opened from the reader answers false here but true above.
  bool currentIsReaderActivity() const;
  bool skipLoopDelay() const;

  // True while the current activity owns the raw serial input stream (see
  // Activity::ownsSerialInput()). main.cpp consults this to suppress its
  // line-based `CMD:` serial reader so it doesn't steal protocol bytes.
  bool currentOwnsSerialInput() const;

  // Forward to the current activity so it can redraw the visible screen into the frame
  // buffer before a raw capture (e.g. a screenshot). See Activity::prepareFramebufferForCapture().
  void prepareFramebufferForCapture();

  // Dispatch a globally-configured button action to the current activity.
  // Reader-specific actions (page navigation, TOC, bookmarks, footnotes) are forwarded
  // only when the current activity is a reader; others are no-ops in other contexts.
  void dispatchButtonAction(CrossPointSettings::BUTTON_ACTION action);

  // Offer a manual force refresh to the activity on top. False when it leaves the refresh to the
  // caller -- see Activity::handleForcedRefresh().
  bool handleForcedRefresh(HalDisplay::RefreshMode mode);

#if CP_TOUCH_UI
  // Pull the reading-light submenu down from the top edge, from whatever screen is up.
  // Global rather than reader-only on purpose: reaching the light from the home screen in
  // the dark is most of what a quick route to it is for, and taps/long taps are reader-only
  // so a swipe is the only gesture that can serve both.
  void dispatchLightPanelGesture();
  // Turn a tap on the on-screen button-hint strip into the button press it depicts, so the
  // four labels at the bottom work as touch targets on every screen that draws them. Runs
  // after the current activity's loop() so screens that handle touch themselves win; see
  // the definition for why that ordering matters.
  void dispatchListTap();
  // A vertical swipe over a painted list pages it, by injecting the list page button. See the
  // definition for why this is a synthesis rather than a per-screen handler.
  void dispatchListSwipe();
  void dispatchHintStripTap();
#endif  // CP_TOUCH_UI

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();

  // True when a further render has already been requested while the current one is still
  // running — the frame being rendered is superseded before it is even on screen. Intended
  // to be called FROM the render task, as late as possible, so a long but purely cosmetic
  // tail of a render (the reader's anti-aliasing touch-up) can be dropped for a page that
  // is about to be replaced. Never skip anything the next frame depends on: the answer is
  // advisory and can flip to true a moment after it is read.
  bool isUpdateSuperseded() const;
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
