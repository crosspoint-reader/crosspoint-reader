#pragma once

#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "RosaryData.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RosaryPrayerReferenceActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool showingPrayerText = false;
  int selectedPrayer = 0;

  const std::function<void()> onComplete;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void drawWrappedText(int fontId, int x, int y, int maxWidth, int maxHeight, const char* text,
                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

 public:
  explicit RosaryPrayerReferenceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const std::function<void()>& onComplete)
      : Activity("RosaryPrayers", renderer, mappedInput), onComplete(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
