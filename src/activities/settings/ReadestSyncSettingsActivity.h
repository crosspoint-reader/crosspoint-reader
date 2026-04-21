#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for Readest Sync.
 *
 * Readest uses the KOReader sync protocol. Users must provide a
 * self-hosted KOReader-compatible sync server URL. Readest does not
 * host a public server.
 */
class ReadestSyncSettingsActivity final : public Activity {
 public:
  explicit ReadestSyncSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadestSyncSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
