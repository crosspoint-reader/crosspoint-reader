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
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
  bool confirmingDelete = false;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : Activity("EpubReaderBookmarks", renderer, mappedInput), epub(epub), epubPath(epubPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the bookmark list based on orientation
  int getListHeight(const GfxRenderer& renderer);
};
