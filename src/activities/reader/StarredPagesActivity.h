#pragma once

#include <Epub.h>

#include <memory>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class StarredPagesActivity final : public Activity {
  std::shared_ptr<Epub> epub;  // nullptr for TXT files
  const std::vector<BookmarkStore::Bookmark> bookmarks;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  int getPageItems() const;
  std::string getItemLabel(int index) const;

 public:
  explicit StarredPagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::vector<BookmarkStore::Bookmark>& bookmarks,
                                std::shared_ptr<Epub> epub = nullptr)
      : Activity("StarredPages", renderer, mappedInput), epub(std::move(epub)), bookmarks(bookmarks) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
