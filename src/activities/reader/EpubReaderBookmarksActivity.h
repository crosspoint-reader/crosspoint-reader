#pragma once
#include <Epub.h>

#include <memory>

#include "../../util/BookmarkUtil.h"
#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  BookmarkUtil bookmarkUtil;
  int currentSpineIndex = 0;
  int currentPage = 0;
  int pageCount = 0;
  int selectorIndex = 0;
  std::string pageText;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

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
        pageText(pageText),
        bookmarkUtil(BookmarkUtil(epub, epubPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
