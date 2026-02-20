#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

class ClippingTextViewerActivity final : public ActivityWithSubactivity {
  std::string text;
  std::vector<std::string> lines;  // text split into screen-width lines
  int scrollOffset = 0;            // first visible line index
  int linesPerPage = 0;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  const std::function<void()> onGoBack;

  void wrapText();  // Split text into lines that fit screen width

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit ClippingTextViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                      const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("ClippingTextViewer", renderer, mappedInput),
        text(std::move(text)),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
