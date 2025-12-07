#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "Screen.h"

class EpubReaderScreen final : public Screen {
  Epub* epub;
  Section* section = nullptr;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(const Page *p);
  void renderStatusBar() const;

 public:
  explicit EpubReaderScreen(EpdRenderer& renderer, InputManager& inputManager, Epub* epub,
                            const std::function<void()>& onGoHome)
      : Screen(renderer, inputManager), epub(epub), onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void handleInput() override;
};
