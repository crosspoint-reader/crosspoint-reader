#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for Koofr highlight sync settings.
 * Shows the account e-mail, app password, destination folder and WebDAV URL.
 * There is no separate connection test: configuring credentials adds the
 * "Sync Highlights" entry to the home menu, and running it reports the result.
 */
class KoofrSettingsActivity final : public Activity {
 public:
  explicit KoofrSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KoofrSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  void handleSelection();
};
