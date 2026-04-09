#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Base class for FreeRTOS tasks with lifecycle management.
// Subclasses implement run(); owners call start()/stop()/wait().
class Task {
  TaskHandle_t handle = nullptr;
  volatile bool stopRequested = false;

  static void trampoline(void* p) {
    auto* self = static_cast<Task*>(p);
    self->run();
    self->handle = nullptr;
    vTaskDelete(nullptr);
  }

 public:
  virtual ~Task() {
    stop();
    wait();
  }

  void start(const char* name, uint32_t stackBytes, uint8_t priority) {
    stopRequested = false;
    xTaskCreate(trampoline, name, stackBytes, this, priority, &handle);
  }

  void stop() { stopRequested = true; }

  void wait() {
    while (handle) vTaskDelay(1);
  }

  bool isRunning() const { return handle != nullptr; }
  bool shouldStop() const { return stopRequested; }

 protected:
  virtual void run() = 0;
};
