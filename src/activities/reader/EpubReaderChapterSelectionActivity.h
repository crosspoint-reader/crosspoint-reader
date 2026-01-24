#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../ActivityWithSubactivity.h"

class EpubReaderChapterSelectionActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
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

  // Total items including sync options (top and bottom)
  int getTotalItems() const;

  // Check if sync option is available (credentials configured)
  bool hasSyncOption() const;

  // Check if given item index is a sync option (first or last)
  bool isSyncItem(int index) const;

  // Convert item index to TOC index (accounting for top sync option offset)
  int tocIndexFromItemIndex(int itemIndex) const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void launchSyncActivity();

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
