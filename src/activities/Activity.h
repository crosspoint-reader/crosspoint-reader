#pragma once
#include <HardwareSerial.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <string>
#include <utility>

#include "GfxRenderer.h"
#include "MappedInputManager.h"

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  [[noreturn]] static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Mutex to protect rendering operations from being deleted mid-render
  SemaphoreHandle_t renderingMutex = nullptr;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
  }
  virtual ~Activity() {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  };
  class RenderLock;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}
  virtual void requestUpdate();
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }

  // RAII helper to lock rendering mutex for the duration of a scope.
  class RenderLock {
    Activity& activity;

   public:
    explicit RenderLock(Activity& activity);
    RenderLock(const RenderLock&) = delete;
    RenderLock& operator=(const RenderLock&) = delete;
    ~RenderLock();
  };
};
