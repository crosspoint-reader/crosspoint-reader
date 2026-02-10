#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for testing KOReader credentials.
 * Connects to WiFi and authenticates with the KOReader sync server.
 */
class KOReaderAuthActivity final : public ActivityWithSubactivity {
 public:
  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& onComplete)
      : ActivityWithSubactivity("KOReaderAuth", renderer, mappedInput), onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  const std::function<void()> onComplete;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
};
