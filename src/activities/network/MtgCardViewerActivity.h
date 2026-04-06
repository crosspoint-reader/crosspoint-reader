#pragma once

#include <string>

#include "activities/Activity.h"

class MtgCardViewerActivity final : public Activity {
 private:
  enum class ViewerState {
    VIEWING,
    CHECK_WIFI,
    WIFI_SELECTION,
    SYNCING
  };

  ViewerState state = ViewerState::VIEWING;
  std::string statusMessage;
  
  bool isPaused = false;
  unsigned long lastSyncTime = 0;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void performSync();
  unsigned long getIntervalMs() const;
  void renderCard() const;

 public:
  explicit MtgCardViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
