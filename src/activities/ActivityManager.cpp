#include "ActivityManager.h"

#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "core/features/FeatureModules.h"
#include "home/HomeActivity.h"
#include "home/MyLibraryActivity.h"
#include "home/NotesActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServer.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SettingsActivity.h"
#include "todo/TodoPlannerStorage.h"
#include "util/DateUtils.h"
#include "util/FullScreenMessageActivity.h"

ActivityManager::ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
  if (renderingMutex == nullptr) {
    LOG_ERR("ACT", "Failed to create rendering mutex");
    return;
  }
  stackActivities.reserve(10);
}

ActivityManager::~ActivityManager() {
  if (renderTaskHandle != nullptr) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }
  if (renderingMutex != nullptr) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

bool ActivityManager::begin() {
  if (renderTaskHandle != nullptr) {
    return true;
  }
  if (renderingMutex == nullptr) {
    LOG_ERR("ACT", "Cannot create render task without a rendering mutex");
    return false;
  }
  xTaskCreate(&renderTaskTrampoline, "ActivityManagerRender",
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  if (renderTaskHandle == nullptr) {
    LOG_ERR("ACT", "Failed to create render task");
    return false;
  }
  return true;
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
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  // Pump the background web server if running (handles Calibre/KOReader auto-sync)
  if (backgroundServer && backgroundServer->isRunning()) {
    backgroundServer->handleClient();
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
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

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
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate) {
    requestedUpdate = false;
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

void ActivityManager::goToFileBrowser(std::string path) { goToMyLibrary(std::move(path)); }

void ActivityManager::goToMyLibrary(std::string path) {
  replaceActivity(std::make_unique<MyLibraryActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToNotes() { replaceActivity(std::make_unique<NotesActivity>(renderer, mappedInput)); }

void ActivityManager::goToBrowser() {
  replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput));
}

void ActivityManager::goToTodo() {
  if (!core::FeatureModules::shouldExposeHomeAction(core::HomeOptionalAction::TodoPlanner, false)) {
    return;
  }

  const std::string today = DateUtils::currentDate();
  if (today.empty()) {
    // NTP not synced yet — use a fixed "undated" file so the planner is still usable.
    const bool mdEnabled = core::FeatureModules::hasCapability(core::Capability::MarkdownSupport);
    const std::string fallbackPath = std::string("/daily/undated") + (mdEnabled ? ".md" : ".txt");
    if (Activity* todo = core::FeatureModules::createTodoPlannerActivity(renderer, mappedInput, fallbackPath, "Undated",
                                                                         [] { activityManager.goHome(); })) {
      replaceActivity(std::unique_ptr<Activity>(todo));
    } else {
      goHome();
    }
    return;
  }

  const std::string todoMdPath = "/daily/" + today + ".md";
  const std::string todoTxtPath = "/daily/" + today + ".txt";
  const bool todoMdExists = Storage.exists(todoMdPath.c_str());
  const bool todoTxtExists = Storage.exists(todoTxtPath.c_str());
  if (todoMdExists || todoTxtExists) {
    if (Activity* todo = core::FeatureModules::createTodoPlannerActivity(
            renderer, mappedInput,
            TodoPlannerStorage::dailyPath(today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport),
                                          todoMdExists, todoTxtExists),
            today, [] { activityManager.goHome(); })) {
      replaceActivity(std::unique_ptr<Activity>(todo));
    } else {
      goHome();
    }
    return;
  }

  const std::string todoEpubPath = "/daily/" + today + ".epub";
  if (Storage.exists(todoEpubPath.c_str())) {
    goToReader(todoEpubPath);
    return;
  }

  if (Activity* todo = core::FeatureModules::createTodoPlannerActivity(
          renderer, mappedInput,
          TodoPlannerStorage::dailyPath(today, core::FeatureModules::hasCapability(core::Capability::MarkdownSupport),
                                        todoMdExists, todoTxtExists),
          today, [] { activityManager.goHome(); })) {
    replaceActivity(std::unique_ptr<Activity>(todo));
  } else {
    goHome();
  }
}

void ActivityManager::goToReader(std::string path) {
  auto result = core::FeatureModules::createReaderActivityForPath(
      path, renderer, mappedInput,
      [path](const std::string& bookPath) {
        const auto slash = bookPath.rfind('/');
        const std::string folder = (slash != std::string::npos && slash > 0) ? bookPath.substr(0, slash) : "/";
        activityManager.goToMyLibrary(folder);
      },
      [] { activityManager.goHome(); });

  if (result.status == core::FeatureModules::ReaderOpenStatus::Opened && result.activity) {
    replaceActivity(std::unique_ptr<Activity>(result.activity));
  } else {
    if (result.logMessage) {
      LOG_ERR("ACT", "Cannot open reader: %s", result.logMessage);
    }
    goHome();
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

void ActivityManager::goHome() { replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
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

bool ActivityManager::skipLoopDelay() const {
  if (backgroundServer && backgroundServer->isRunning()) return true;
  return currentActivity && currentActivity->skipLoopDelay();
}

void ActivityManager::startBackgroundWebServer(std::unique_ptr<CrossPointWebServer>&& server) {
  stopBackgroundWebServer();
  backgroundServer = std::move(server);
  LOG_DBG("ACT", "Background web server started");
}

void ActivityManager::stopBackgroundWebServer() {
  if (backgroundServer) {
    backgroundServer->stop();
    backgroundServer.reset();
    LOG_DBG("ACT", "Background web server stopped");
  }
}

bool ActivityManager::hasBackgroundWebServer() const { return backgroundServer && backgroundServer->isRunning(); }

CrossPointWebServer* ActivityManager::getBackgroundWebServer() const { return backgroundServer.get(); }

bool ActivityManager::blocksBackgroundServer() const {
  return currentActivity && currentActivity->blocksBackgroundServer();
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}

void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  taskENTER_CRITICAL(nullptr);
  const auto currTaskHandler = xTaskGetCurrentTaskHandle();
  const auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  const bool isRenderTask = (currTaskHandler == renderTaskHandle);
  const bool alreadyWaiting = (waitingTaskHandle != nullptr);
  const bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(nullptr);

  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");
  assert(!alreadyWaiting && "Already waiting for a render to complete");
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
