#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * Submenu for RemoteLibrary settings.
 * Shows RemoteLibrary Web URL and Calibre Wireless Device options.
 */
class RemoteLibrarySettingsActivity final : public ActivityWithSubactivity {
 public:
  explicit RemoteLibrarySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onBack)
      : ActivityWithSubactivity("RemoteLibrarySettings", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  int selectedIndex = 0;
  const std::function<void()> onBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void handleSelection();
};
