#pragma once

#include <vector>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MtgSyncActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  enum class SyncState {
    MENU,
    CHECK_WIFI,
    WIFI_SELECTION,
    SYNCING,
    SHOW_MESSAGE
  };

  SyncState state = SyncState::MENU;
  std::string statusMessage;
  unsigned long messageStartTime = 0;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void performSync();

 public:
  explicit MtgSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MtgSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
