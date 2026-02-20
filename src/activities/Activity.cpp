#include "Activity.h"
#include "ActivityManager.h"

void Activity::renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  self->renderTaskLoop();
}

void Activity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      RenderLock lock(*this);
      render(std::move(lock));
    }
  }
}

void Activity::onEnter() {
  xTaskCreate(&renderTaskTrampoline, name.c_str(),
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  RenderLock lock(*this);  // Ensure we don't delete the task while it's rendering
  if (renderTaskHandle) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }

  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}

void Activity::requestUpdateAndWait() {
  // FIXME @ngxson : properly implement this using freeRTOS notification
  delay(100);
}

void Activity::onGoHome() {
  activityManager.goHome();
}

void Activity::onSelectBook(const std::string& path) {
  Intent intent;
  intent.path = path;
  activityManager.goToReader(std::move(intent));
}

// RenderLock

Activity::RenderLock::RenderLock(Activity& activity) : activity(activity) {
  xSemaphoreTake(activity.renderingMutex, portMAX_DELAY);
}

Activity::RenderLock::~RenderLock() { xSemaphoreGive(activity.renderingMutex); }
