#pragma once

#include <functional>

#include "OpdsServerStore.h"
#include "activities/ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

/**
 * Edit screen for a single OPDS server.
 * Shows Name, URL, Username, Password fields and a Delete option.
 * Used for both adding new servers and editing existing ones.
 */
class CalibreSettingsActivity final : public ActivityWithSubactivity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit CalibreSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onBack, int serverIndex = -1)
      : ActivityWithSubactivity("CalibreSettings", renderer, mappedInput), onBack(onBack), serverIndex(serverIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  const std::function<void()> onBack;
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;

  int getMenuItemCount() const;
  void handleSelection();
  void saveServer();
};
