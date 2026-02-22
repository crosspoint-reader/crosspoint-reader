#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for authenticating or registering a KOReader sync account.
 * Connects to WiFi, then either logs in or creates a new account on the server.
 */
class KOReaderAuthActivity final : public ActivityWithSubactivity {
 public:
  /** PROMPT shows the idle screen so the user can choose; LOGIN/REGISTER skip straight to the action. */
  enum class Mode { LOGIN, REGISTER, PROMPT };

  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& onComplete, Mode startMode = Mode::PROMPT)
      : ActivityWithSubactivity("KOReaderAuth", renderer, mappedInput), onComplete(onComplete), mode(startMode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING || state == REGISTERING; }

 private:
  enum State { IDLE, WIFI_SELECTION, CONNECTING, AUTHENTICATING, REGISTERING, SUCCESS, FAILED, USER_EXISTS };

  State state = IDLE;
  Mode mode = Mode::PROMPT;
  std::string statusMessage;
  std::string errorMessage;

  const std::function<void()> onComplete;

  void onWifiSelectionComplete(bool success);
  void startWifi();
  void performAuthentication();
  void performRegistration();
};
