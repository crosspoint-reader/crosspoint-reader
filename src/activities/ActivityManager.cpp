#include "ActivityManager.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/GlobalBookmarksActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/KOReaderSyncActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"
#include "weather/WeatherActivity.h"

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

void ActivityManager::begin() {
  xTaskCreate(&renderTaskTrampoline, "ActivityManagerRender",
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

#if DEBUG_MEMORY_CONSUMPTION
static void logActivityStackState(const char* stage, Activity* currentActivity, size_t stackSize) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ACT", "%s: current=%s stackSize=%zu free=%lu contig=%lu", stage,
          currentActivity ? currentActivity->getName().c_str() : "<none>", stackSize, freeHeap, contigHeap);
}
#else
static inline void logActivityStackState(const char*, Activity*, size_t) {}
#endif

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(nullptr);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(nullptr);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  // Drain leftover input after an activity transition so that the button press/release
  // used to leave one activity cannot bleed into the next.  We consume events until
  // every button is released and no press/release edges remain.
  if (drainInput) {
    if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() ||
        mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.isPressed(MappedInputManager::Button::Left) ||
        mappedInput.isPressed(MappedInputManager::Button::Right) ||
        mappedInput.isPressed(MappedInputManager::Button::Up) ||
        mappedInput.isPressed(MappedInputManager::Button::Down)) {
      // Still have pending input — skip the activity loop but continue with
      // the rest (pending-action processing, render flushing) so that
      // transitions and screen updates are not delayed.
    } else {
      drainInput = false;
    }
  }

  if (!drainInput && currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  if (SETTINGS.useClock && HalClock::isSynced()) {
    time_t now = HalClock::now();
    if (now > 0) {
      static time_t lastMinute = -1;
      time_t minute = now / 60;
      if (minute != lastMinute) {
        lastMinute = minute;
        requestUpdate();
      }
    }
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, returning from child");
        lock.unlock();  // returnFromChild may acquire its own lock via replaceActivity
        returnFromChild();
        continue;  // Will launch the target activity immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Arm input drain so the button that triggered the pop doesn't bleed into the
        // restored activity (or into a new activity the handler just pushed).
        drainInput = true;

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("replace_before", currentActivity.get(), stackActivities.size());
#endif
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("replace_after_clear", nullptr, stackActivities.size());
#endif
      } else if (pendingAction == PendingAction::Push) {
#if DEBUG_MEMORY_CONSUMPTION
        logActivityStackState("push_before", currentActivity.get(), stackActivities.size());
#endif
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
#if DEBUG_MEMORY_CONSUMPTION
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
        logActivityStackState("push_after", currentActivity.get(), stackActivities.size());
#else
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
#endif
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // Arm input drain so the button that triggered the transition doesn't bleed
      // into the new activity.
      drainInput = true;

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate) {
    taskENTER_CRITICAL(nullptr);
    requestedUpdate = false;
    taskEXIT_CRITICAL(nullptr);
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
#if DEBUG_MEMORY_CONSUMPTION
    LOG_DBG("ACT", "replaceActivity requested: current=%s stackSize=%zu", currentActivity->getName().c_str(),
            stackActivities.size());
#endif
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path, std::string focusName) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path), std::move(focusName)));
}

void ActivityManager::goToRecentBooks(int focusIndex) {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput, focusIndex));
}

void ActivityManager::goToGlobalBookmarks() { goToGlobalBookmarks({}); }

void ActivityManager::goToGlobalBookmarks(ReturnHint hint) {
  hasReturnHint = false;
  replaceActivity(std::make_unique<GlobalBookmarksActivity>(renderer, mappedInput, std::move(hint)));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path) {
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToKOReaderSync() {
  const auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || sync.epubPath.empty()) {
    LOG_ERR("ACT", "Cannot launch KOReader sync without an active EPUB handoff");
    goHome();
    return;
  }

  replaceActivity(std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, sync.epubPath, sync.spineIndex,
                                                         sync.page, sync.totalPagesInSpine, sync.paragraphIndex,
                                                         sync.hasParagraphIndex, sync.xhtmlSeekHint, sync.intent));
}

void ActivityManager::replaceWithReader(std::string path, ReturnHint hint) {
  returnHint = std::move(hint);
  hasReturnHint = true;
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::replaceWithFileBrowser(std::string path, ReturnHint hint, std::string focusName) {
  returnHint = std::move(hint);
  hasReturnHint = true;
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path), std::move(focusName)));
}

void ActivityManager::replaceWithRecentBooks(ReturnHint hint) {
  returnHint = std::move(hint);
  hasReturnHint = true;
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput, -1));
}

void ActivityManager::returnFromChild() {
  if (!hasReturnHint) {
    goHome();
    return;
  }
  ReturnHint hint = std::move(returnHint);
  returnHint = {};
  hasReturnHint = false;

  switch (hint.target) {
    case ReturnTo::FileBrowser:
      goToFileBrowser(std::move(hint.path), std::move(hint.selectName));
      break;
    case ReturnTo::RecentBooks:
      goToRecentBooks(hint.selectIndex);
      break;
    case ReturnTo::GlobalBookmarks:
      goToGlobalBookmarks(std::move(hint));
      break;
    case ReturnTo::Home:
    default:
      goHome(std::move(hint.selectName), hint.selectIndex);
      break;
  }
}

void ActivityManager::goToSleep() {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goToWeather() { replaceActivity(std::make_unique<WeatherActivity>(renderer, mappedInput)); }

void ActivityManager::goHome(std::string focusBookPath, int focusSelectorIndex) {
  hasReturnHint = false;
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, std::move(focusBookPath), focusSelectorIndex));
}

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
#if DEBUG_MEMORY_CONSUMPTION
  LOG_DBG("ACT", "pushActivity requested: current=%s stackSize=%zu",
          currentActivity ? currentActivity->getName().c_str() : "<none>", stackActivities.size());
#endif
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    taskENTER_CRITICAL(nullptr);
    requestedUpdate = true;
    taskEXIT_CRITICAL(nullptr);
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(nullptr);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(nullptr);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
