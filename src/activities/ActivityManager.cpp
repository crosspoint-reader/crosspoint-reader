#include "ActivityManager.h"

#include <HalPowerManager.h>

#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/MyLibraryActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

void ActivityManager::begin() {
  xTaskCreate(&renderTaskTrampoline, "ActivityManagerRender",
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

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
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != None) {
    if (pendingAction == Pop) {
      RenderLock lock;
      ActivityResult pendingResult = currentActivity->result;  // copy before we destroy the activity

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = stackActivities.back();
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %d", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          delete stackActivities.back();
          stackActivities.pop_back();
        }
      } else if (pendingAction == Push) {
        // Move current activity to stack
        stackActivities.push_back(currentActivity);
        LOG_DBG("ACT", "Pushed to activity stack, new size = %d", stackActivities.size());
      }
      pendingAction = None;
      currentActivity = pendingActivity;
      pendingActivity = nullptr;

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }
}

void ActivityManager::exitActivity(RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void ActivityManager::replaceActivity(Activity* newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity leading to the "delete
    // this" problem
    pendingActivity = newActivity;
    pendingAction = Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = newActivity;
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() { replaceActivity(new CrossPointWebServerActivity(renderer, mappedInput)); }

void ActivityManager::goToSettings() { replaceActivity(new SettingsActivity(renderer, mappedInput)); }

void ActivityManager::goToMyLibrary(Intent&& intent) {
  replaceActivity(new MyLibraryActivity(renderer, mappedInput, intent.path));
}

void ActivityManager::goToRecentBooks() { replaceActivity(new RecentBooksActivity(renderer, mappedInput)); }

void ActivityManager::goToBrowser() { replaceActivity(new OpdsBookBrowserActivity(renderer, mappedInput)); }

void ActivityManager::goToReader(Intent&& intent) {
  replaceActivity(new ReaderActivity(renderer, mappedInput, intent.path));
}

void ActivityManager::goToSleep() {
  replaceActivity(new SleepActivity(renderer, mappedInput));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(new BootActivity(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(Intent&& intent) {
  replaceActivity(new FullScreenMessageActivity(renderer, mappedInput, intent.message, intent.messageStyle));
}

void ActivityManager::goHome() { replaceActivity(new HomeActivity(renderer, mappedInput)); }

void ActivityManager::pushActivity(Activity* activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    delete pendingActivity;
    pendingActivity = nullptr;
  }
  pendingActivity = activity;
  pendingAction = Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    delete pendingActivity;
    pendingActivity = nullptr;
  }
  pendingAction = Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

void ActivityManager::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock(Activity& /* unused */) {
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
