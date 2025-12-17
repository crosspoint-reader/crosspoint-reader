#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../Activity.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(int newSpineIndex)> onSelectSpineIndex;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, InputManager& inputManager,
                                              const std::shared_ptr<Epub>& epub, const int currentSpineIndex,
                                              const std::function<void()>& onGoBack,
                                              const std::function<void(int newSpineIndex)>& onSelectSpineIndex)
      : Activity(renderer, inputManager),
        epub(epub),
        currentSpineIndex(currentSpineIndex),
        onGoBack(onGoBack),
        onSelectSpineIndex(onSelectSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
