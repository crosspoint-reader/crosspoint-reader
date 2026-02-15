#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, and authenticate options.
 */
class KOReaderSettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::function<void()>& onBack)
      : ActivityWithSubactivity("KOReaderSettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  size_t selectedIndex = 0;
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleSelection();
};
