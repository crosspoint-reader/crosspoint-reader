#pragma once

#include "activities/Activity.h"

class SyncTimeActivity final : public Activity {
 public:
  explicit SyncTimeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SyncTime", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { CONNECTING, SYNCING, SUCCESS, FAILED };
  State state = CONNECTING;
  void onWifiSelectionComplete(bool success);
  void onWifiSelectionCancelled();
  void performSync();
};
