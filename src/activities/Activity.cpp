#include "Activity.h"

void Activity::renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  return self->renderTaskLoop();
}

void Activity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    {
      RenderLock lock(*this);
      render();
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
  Serial.printf("[%lu] [ACT] Entering activity: %s\n", millis(), name.c_str());
}

void Activity::onExit() {
  RenderLock lock(*this);  // Ensure we don't delete the task while it's rendering
  vTaskDelete(renderTaskHandle);
  renderTaskHandle = nullptr;

  Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}

// RenderLock

Activity::RenderLock::RenderLock(Activity& activity) : activity(activity) {
  xSemaphoreTake(activity.renderingMutex, portMAX_DELAY);
}

Activity::RenderLock::~RenderLock() { xSemaphoreGive(activity.renderingMutex); }
