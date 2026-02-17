#pragma once

#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#include "RosaryData.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RosaryPrayerActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  bool updateRequired = false;

  RosaryData::DayOfWeek day;
  RosaryData::MysterySet mysterySet;
  int currentStep = 0;

  const std::function<void()> onComplete;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  // Text wrapping helper
  void drawWrappedText(int fontId, int x, int y, int maxWidth, int maxHeight, const char* text,
                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Get display info for current step
  std::string getStepTitle() const;
  std::string getStepSubtitle() const;
  const char* getStepPrayerText() const;
  std::string getProgressText() const;

  // Draw rosary bead visualization
  void drawBeadVisualization(int x, int y, int width, int height) const;

 public:
  explicit RosaryPrayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, RosaryData::DayOfWeek day,
                                const std::function<void()>& onComplete)
      : Activity("RosaryPrayer", renderer, mappedInput), day(day), onComplete(onComplete) {
    mysterySet = RosaryData::getMysterySetForDay(day);
  }
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
