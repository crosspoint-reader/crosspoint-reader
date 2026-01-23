#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class SleepBmpSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::vector<std::string> files;  // Sorted list of valid BMP filenames ("Random" at index 0)
  size_t selectorIndex = 0;
  bool updateRequired = false;
  unsigned long enterTime = 0;  // Time when activity was entered
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();  // Load and sort all valid BMP files

 public:
  explicit SleepBmpSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      const std::function<void()>& onBack)
      : Activity("SleepBmpSelection", renderer, mappedInput), onBack(onBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};

