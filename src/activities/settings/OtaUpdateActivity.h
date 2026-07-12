#pragma once

#include "activities/Activity.h"
#include "network/OtaBootCheck.h"
#include "network/OtaUpdater.h"

// The network work (check + install) runs at boot time via OtaBootCheck, not
// in this activity — see OtaBootCheck.h. This activity only shows the boot
// result, collects the user's confirmation, and requests the next boot stage.
class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    NO_UPDATE,
    FAILED,
  };

  State state = WIFI_SELECTION;
  OtaUpdater updater;

  void onWifiSelectionComplete(bool success);
  void consumeBootResult(const OtaBootCheck::Result& result);

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
