#pragma once

#include "activities/Activity.h"

/**
 * Tests Readest sync credentials by connecting to the configured server.
 */
class ReadestAuthActivity final : public Activity {
 public:
  explicit ReadestAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadestAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
};
