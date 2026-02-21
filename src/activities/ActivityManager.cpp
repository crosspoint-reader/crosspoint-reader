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
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      RenderLock lock;
      currentActivity->render(std::move(lock));
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  if (pendingAction == Pop) {
    RenderLock lock;
    if (stackActivities.empty()) {
      goHome();
    } else {
      // Destroy the current activity
      exitActivity();
      currentActivity = stackActivities.back();
      stackActivities.pop_back();
      // Handle result if necessary
      if (currentActivity->resultHandler) {
        // Move the result handler out of the activity before calling it, to avoid potential issues if the handler tries
        // to launch a new activity
        auto handler = std::move(currentActivity->resultHandler);
        currentActivity->resultHandler = nullptr;
        handler(pendingResult);
      }
    }

  } else if (pendingActivity) {
    // Current activity has requested a new activity to be launched
    RenderLock lock;

    if (pendingAction == Replace) {
      // Destroy the current activity
      exitActivity();
      // Clear the stack
      while (!stackActivities.empty()) {
        stackActivities.back()->onExit();
        delete stackActivities.back();
        stackActivities.pop_back();
      }
    } else if (pendingAction == Push) {
      // Move current activity to stack
      stackActivities.push_back(currentActivity);
    }
    currentActivity = pendingActivity;
    pendingActivity = nullptr;
    currentActivity->onEnter();
  }
}

void ActivityManager::exitActivity() {
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
  pendingActivity = activity;
  pendingAction = Push;
}

void ActivityManager::pushActivityForResult(Activity* activity, std::function<void(ActivityResult&)> resultHandler) {
  activity->resultHandler = std::move(resultHandler);
  pushActivity(activity);
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

void ActivityManager::popActivityWithResult(ActivityResult& result) {
  pendingResult = result;  // copy
  popActivity();
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

RenderLock::RenderLock() { xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY); }

RenderLock::RenderLock(Activity& /* unused */) { xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY); }

RenderLock::~RenderLock() { xSemaphoreGive(activityManager.renderingMutex); }
