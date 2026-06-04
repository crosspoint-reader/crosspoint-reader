#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RssFeedListActivity final : public Activity {
 public:
  explicit RssFeedListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false)
      : Activity("RssFeedList", renderer, mappedInput), pickerMode(pickerMode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  bool pickerMode = false;
  int selectedIndex = 0;

  int getItemCount() const;
  void handleSelection();
};
