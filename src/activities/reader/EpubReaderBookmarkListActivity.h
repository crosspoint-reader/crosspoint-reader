#pragma once
#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "BookmarkStore.h"

class EpubReaderBookmarkListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<BookmarkEntry> bookmarks;
  int selectorIndex = 0;
  bool confirmingDelete = false;

  const std::function<std::string(uint16_t)> resolveChapterTitle;
  const std::function<void()> onGoBack;
  const std::function<void(int spineIndex, int pageIndex)> onSelectBookmark;

  int getPageItems() const;
  int getTotalItems() const;

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
  void render(Activity::RenderLock&&) override;
};
