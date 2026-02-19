#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * @brief Activity to handle TRMNL device registration.
 *
 * This activity manages the following flow:
 * 1. Connect to WiFi (using WifiSelectionActivity).
 * 2. On success, attempt to register the device with the TRMNL server.
 * 3. Display the result (success or failure) to the user.
 */
class TrmnlRegistrationActivity final : public ActivityWithSubactivity {
 public:
  using OnFinishCallback = std::function<void()>;

  explicit TrmnlRegistrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     OnFinishCallback onFinish);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State {
    WIFI_SELECTION,
    REGISTERING,
    SUCCESS,
    FAILURE,
  };

  OnFinishCallback onFinish;
  State state = State::WIFI_SELECTION;
  std::string statusMessage;

  /**
   * @brief Callback executed after the WiFi selection sub-activity completes.
   * @param success True if WiFi connection was successful.
   */
  void onWifiSelectionComplete(bool success);
};