#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "RosaryData.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RosaryMysteryListActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool updateRequired = false;

  RosaryData::DayOfWeek day;
  RosaryData::MysterySet currentSet;
  bool showingAllSets = false;

  const std::function<void()> onComplete;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit RosaryMysteryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     RosaryData::DayOfWeek day, const std::function<void()>& onComplete)
      : Activity("RosaryMysteries", renderer, mappedInput),
        day(day),
        currentSet(RosaryData::getMysterySetForDay(day)),
        onComplete(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
