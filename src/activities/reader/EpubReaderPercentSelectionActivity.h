#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "MappedInputManager.h"
#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public ActivityWithSubactivity {
 public:
  // Slider-style percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent, const std::function<void(int)>& onSelect,
                                              const std::function<void()>& onCancel)
      : ActivityWithSubactivity("EpubReaderPercentSelection", renderer, mappedInput),
        percent(initialPercent),
        onSelect(onSelect),
        onCancel(onCancel) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  // Current percent value (0-100) shown on the slider.
  int percent = 0;
  // Render dirty flag for the task loop.
  bool updateRequired = false;
  // FreeRTOS task and mutex for rendering.
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;

  // Callback invoked when the user confirms a percent.
  const std::function<void(int)> onSelect;
  // Callback invoked when the user cancels the slider.
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  // Render the slider UI.
  void renderScreen();
  // Change the current percent by a delta and clamp within bounds.
  void adjustPercent(int delta);
};
