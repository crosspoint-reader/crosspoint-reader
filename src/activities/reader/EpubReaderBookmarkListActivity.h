#pragma once
#include <functional>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"

class EpubReaderBookmarkListActivity final : public Activity {
  std::string bookPath;
  std::vector<BookmarkEntry> bookmarks;
  int selectorIndex = 0;
  bool confirmingDelete = false;

  const std::function<std::string(uint16_t)> resolveChapterTitle;

  int getPageItems() const;
  int getTotalItems() const;

 public:
  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::string& bookPath,
                                          const std::function<std::string(uint16_t)>& resolveChapterTitle)
      : Activity("EpubReaderBookmarkList", renderer, mappedInput),
        bookPath(bookPath),
        resolveChapterTitle(resolveChapterTitle) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
