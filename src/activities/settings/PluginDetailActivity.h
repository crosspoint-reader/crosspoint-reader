#pragma once

#include "activities/Activity.h"

/**
 * Shows full detail for a single plugin: name, author, version, description,
 * compatibility, and an enable/disable toggle.  Also dispatches the plugin's
 * own on_settings_render hook so it can inject custom rows.
 */
class PluginDetailActivity final : public Activity {
  int pluginIndex;
  bool pendingFinish_ = false;

 public:
  explicit PluginDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int pluginIndex)
      : Activity("PluginDetail", renderer, mappedInput), pluginIndex(pluginIndex) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
