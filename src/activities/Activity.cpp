#include "Activity.h"

[[noreturn]] static void renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  while (true) {
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    self->render();
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
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  vTaskDelete(renderTaskHandle);
  renderTaskHandle = nullptr;
  xSemaphoreGive(renderingMutex);

  Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}
