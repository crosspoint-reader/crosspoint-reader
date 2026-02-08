#pragma once
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>
#include <utility>

class MappedInputManager;
class GfxRenderer;

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  [[noreturn]] static void renderTaskTrampoline(void* param);
  void renderTaskLoop();
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
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}
  virtual void render() {}
  virtual void requestUpdate();
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
};
