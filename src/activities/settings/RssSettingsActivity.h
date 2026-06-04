#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#include "RssFeedStore.h"

class RssSettingsActivity final : public Activity {
 public:
  RssSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int feedIndex)
      : Activity("RssSettings", renderer, mappedInput), feedIndex(feedIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int feedIndex = -1;
  int selectedIndex = 0;
  bool isNewFeed = true;
  bool showSaveError = false;
  RssFeed editFeed;

  int getMenuItemCount() const;
  bool saveFeed();
  void handleSelection();
};
