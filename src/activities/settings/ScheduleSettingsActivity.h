#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/Activity.h"

/**
 * ScheduleSettingsActivity allows users to configure automatic file transfer server scheduling.
 * Users can set up recurring schedules (hourly, daily) or specific times throughout the week.
 */
class ScheduleSettingsActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  int selectedIndex = 0;  // Currently selected option
  const std::function<void()> onGoBack;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void toggleCurrentSetting();

 public:
  explicit ScheduleSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::function<void()>& onGoBack)
      : Activity("ScheduleSettings", renderer, mappedInput), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
