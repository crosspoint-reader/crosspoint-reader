#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "BookmarkStore.h"

class EpubReaderBookmarkListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<BookmarkEntry> bookmarks;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool confirmingDelete = false;

  const std::function<std::string(uint16_t)> resolveChapterTitle;
  const std::function<void()> onGoBack;
  const std::function<void(int spineIndex, int pageIndex)> onSelectBookmark;

  int getPageItems() const;
  int getTotalItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::string& bookPath,
                                          const std::function<std::string(uint16_t)>& resolveChapterTitle,
                                          const std::function<void()>& onGoBack,
                                          const std::function<void(int spineIndex, int pageIndex)>& onSelectBookmark)
      : ActivityWithSubactivity("EpubReaderBookmarkList", renderer, mappedInput),
        bookPath(bookPath),
        resolveChapterTitle(resolveChapterTitle),
        onGoBack(onGoBack),
        onSelectBookmark(onSelectBookmark) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
