#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "FootnotesData.h"

class EpubReaderTocActivity final : public ActivityWithSubactivity {
 public:
  enum class Tab { CHAPTERS, FOOTNOTES };

 private:
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  const FootnotesData& footnotes;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  int currentSpineIndex = 0;
  int currentPage = 0;
  int totalPagesInSpine = 0;

  Tab currentTab = Tab::CHAPTERS;
  bool updateRequired = false;

  // Chapters tab state
  int chaptersSelectorIndex = 0;
  std::vector<int> filteredSpineIndices;

  // Footnotes tab state
  int footnotesSelectedIndex = 0;

  // Callbacks
  const std::function<void()> onGoBack;
  const std::function<void(int newSpineIndex)> onSelectSpineIndex;
  const std::function<void(const char* href)> onSelectFootnote;
  const std::function<void(int newSpineIndex, int newPage)> onSyncPosition;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

  // Tab-specific methods
  void loopChapters();
  void loopFootnotes();
  void renderChapters(int contentTop, int contentHeight);
  void renderFootnotes(int contentTop, int contentHeight);

  // Chapters helpers
  void buildFilteredChapterList();
  bool hasSyncOption() const;
  bool isSyncItem(int index) const;
  int getChaptersTotalItems() const;
  int getChaptersPageItems(int contentHeight) const;
  int tocIndexFromItemIndex(int itemIndex) const;

  // Indicator helpers
  int getCurrentPage() const;
  int getTotalPages() const;

 public:
  EpubReaderTocActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub_ptr,
                        const std::string& epubPath, int currentSpineIndex, int currentPage, int totalPagesInSpine,
                        const FootnotesData& footnotes, std::function<void()> onGoBack,
                        std::function<void(int)> onSelectSpineIndex, std::function<void(const char*)> onSelectFootnote,
                        std::function<void(int, int)> onSyncPosition)
      : ActivityWithSubactivity("EpubReaderToc", renderer, mappedInput),
        epub(epub_ptr),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        footnotes(footnotes),
        onGoBack(onGoBack),
        onSelectSpineIndex(onSelectSpineIndex),
        onSelectFootnote(onSelectFootnote),
        onSyncPosition(onSyncPosition) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  void launchSyncActivity();
};
