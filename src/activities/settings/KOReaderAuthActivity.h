#pragma once

#include <functional>

#include "activities/Activity.h"

/**
 * Activity for testing KOReader credentials.
 * Connects to WiFi and authenticates with the KOReader sync server.
 */
class KOReaderAuthActivity final : public Activity {
 public:
  enum class Mode { LOGIN, REGISTER };

  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                Mode mode = Mode::LOGIN)
      : Activity("KOReaderAuth", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == AUTHENTICATING || state == REGISTERING;
  }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, REGISTERING, SUCCESS, FAILED, USER_EXISTS };

  Mode mode;
  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
  void performRegistration();
};
