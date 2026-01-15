#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/ActivityWithSubactivity.h"

class FormatSDCardActivity final : public ActivityWithSubactivity {
  enum State { WAITING_CONFIRMATION, FORMATTING, SUCCESS, FAILED };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;
  State state = WAITING_CONFIRMATION;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void performFormat();

 public:
  explicit FormatSDCardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& goBack)
      : ActivityWithSubactivity("FormatSDCard", renderer, mappedInput), goBack(goBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return state == FORMATTING; }
};
