#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BackgroundManagerActivity final : public Activity {
 public:
  explicit BackgroundManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BackgroundManager", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BgItem { std::string name; std::string status; };
  std::vector<BgItem> items;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  void refreshItems();
};
