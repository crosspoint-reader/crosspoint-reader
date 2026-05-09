#pragma once

#include <Epub.h>

#include <memory>
#include <vector>

#include "../Activity.h"
#include "EpubBookmarksStore.h"

class EpubReaderBookmarksActivity final : public Activity {
 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, int currentSpineIndex, int currentPageNumber);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::shared_ptr<Epub> epub;

  std::vector<EpubBookmark> bookmarks;
  int selectorIndex = 0;
  /// True after a delete until user dismisses the "removed" banner (Confirm or Back).
  bool awaitingBookmarkRemovedAck = false;

  int currentSpineIndex = 0;
  int currentPageNumber = 0;

  // Number of items that fit on a page, derived from logical screen height.
  int getPageItems() const;
  int getTotalItems() const;

  int getListStartY() const;
  std::string getBookmarkFullLabel(const EpubBookmark& b) const;
  std::string getBookmarkLabel(const EpubBookmark& b, int maxWidth) const;
  void promptDeleteSelected();
};
