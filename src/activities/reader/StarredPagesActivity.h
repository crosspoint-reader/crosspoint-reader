#pragma once

#include <Epub.h>

#include <memory>
#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class StarredPagesActivity final : public Activity {
  std::shared_ptr<Epub> epub;  // nullptr for TXT files
  BookmarkStore& bookmarkStore;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  std::string getItemLabel(int index) const;
  std::string getDefaultLabel(int index) const;
  void startRename();
  void deleteSelected();

 public:
  explicit StarredPagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, BookmarkStore& bookmarkStore,
                                std::shared_ptr<Epub> epub = nullptr)
      : Activity("StarredPages", renderer, mappedInput), epub(std::move(epub)), bookmarkStore(bookmarkStore) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
