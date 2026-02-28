#pragma once
#include <Epub.h>

#include <memory>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int currentPage = 0;
  int selectorIndex = 0;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

  // Total TOC items count
  int getTotalItems() const;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex, const int currentPage)
      : Activity("EpubReaderBookmarks", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void loadBookmark(int bookmarkIndex);
  void deleteBookmark(int bookmarkIndex);
  // same as overriding
  void saveBookmark(int bookmarkIndex);
};
