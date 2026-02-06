#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentPage = 0;
  int totalPagesInSpine = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void(int newSpineIndex)> onSelectSpineIndex;
  const std::function<void(int newSpineIndex, int newPage)> onSyncPosition;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

  // Total TOC items count
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const int currentPage,
                                              const int totalPagesInSpine, const std::function<void()>& onGoBack,
                                              const std::function<void(int newSpineIndex)>& onSelectSpineIndex,
                                              const std::function<void(int newSpineIndex, int newPage)>& onSyncPosition)
      : ActivityWithSubactivity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        onGoBack(onGoBack),
        onSelectSpineIndex(onSelectSpineIndex),
        onSyncPosition(onSyncPosition) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
