#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "activities/ActivityWithSubactivity.h"

class EpubReaderMenuActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int selectedItemIndex = 0;
  int nextPageNumber = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  
  //Reset to first section to prevent out of bound issue when render setting changes
  const std::function<void()> resetSectionHelper;
  const std::function<void(int newSpineIndex)> onSelectSpineIndex;

  void onSelectChapters();
  void onSelectSettings();

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  const std::shared_ptr<Epub>& epub, const int currentSpineIndex,
                                  const std::function<void()>& onGoBack,
                                  const std::function<void()>& resetSectionHelper,
                                  const std::function<void(int newSpineIndex)>& onSelectSpineIndex)
      : ActivityWithSubactivity("EpubReaderMenu", renderer, mappedInput),
        epub(epub),
        currentSpineIndex(currentSpineIndex),
        onGoBack(onGoBack),
        resetSectionHelper(resetSectionHelper),
        onSelectSpineIndex(onSelectSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
