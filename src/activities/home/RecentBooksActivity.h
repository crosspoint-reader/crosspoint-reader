#pragma once
#include <I18n.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<int8_t> recentBooksFinished;
  bool isBookFinished(size_t index);

  // Data loading
  void loadRecentBooks();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
