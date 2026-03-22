#pragma once
#include <Epub.h>

#include <memory>

#include "../../BookmarkStore.h"
#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentPage = 0;
  int pageCount = 0;
  int selectorIndex = 0;
  std::string pageText;
  std::vector<BookmarkEntry> bookmarks;
  bool confirmingDelete = false;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                       const int currentSpineIndex, const int currentPage, const int pageCount,
                                       const std::string& pageText)
      : Activity("EpubReaderBookmarks", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        pageCount(pageCount),
        pageText(pageText) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
