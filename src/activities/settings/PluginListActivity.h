#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Shows a list of all compiled-in plugins with name, version, and enabled
 * state.  Selecting a row opens the PluginDetailActivity for that plugin.
 */
class PluginListActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

 public:
  explicit PluginListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PluginList", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
