#pragma once

#include "activities/Activity.h"
#include "marginalia/PackageStore.h"
#include "util/ButtonNavigator.h"

class PackageListActivity final : public Activity {
 public:
  explicit PackageListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PackageList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Marginalia::PackageStore packageStore_;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
};
