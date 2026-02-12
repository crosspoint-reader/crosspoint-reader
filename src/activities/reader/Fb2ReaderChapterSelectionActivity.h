#pragma once
#include <Fb2.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../ActivityWithSubactivity.h"

class Fb2ReaderChapterSelectionActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Fb2> fb2;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSectionIndex = 0;
  int currentPage = 0;
  int totalPagesInSection = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(int newSectionIndex)> onSelectSectionIndex;

  int getPageItems() const;
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit Fb2ReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Fb2>& fb2, const int currentSectionIndex,
                                             const int currentPage, const int totalPagesInSection,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(int newSectionIndex)>& onSelectSectionIndex)
      : ActivityWithSubactivity("Fb2ChapterSelection", renderer, mappedInput),
        fb2(fb2),
        currentSectionIndex(currentSectionIndex),
        currentPage(currentPage),
        totalPagesInSection(totalPagesInSection),
        onGoBack(onGoBack),
        onSelectSectionIndex(onSelectSectionIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
