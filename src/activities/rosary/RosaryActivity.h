#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "RosaryData.h"
#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class RosaryActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool updateRequired = false;

  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void startRosary(RosaryData::DayOfWeek day);
  void showMysteryList(RosaryData::DayOfWeek day);
  void showPrayerReference();

 public:
  explicit RosaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                          const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("Rosary", renderer, mappedInput), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
