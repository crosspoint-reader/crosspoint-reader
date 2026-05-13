#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  int selectorIndex = 0;
  int initialFocusIndex = -1;  // applied once in onEnter(), then cleared

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Data loading
  void loadRecentBooks();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int focusIndex = -1)
      : Activity("RecentBooks", renderer, mappedInput), initialFocusIndex(focusIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
