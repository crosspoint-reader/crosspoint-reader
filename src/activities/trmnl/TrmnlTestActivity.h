#pragma once

#include <functional>

#include "activities/ActivityWithSubactivity.h"

/**
 * @brief Activity to test downloading an image from the TRMNL service.
 *
 * This activity manages the following flow:
 * 1. Connect to WiFi (using WifiSelectionActivity).
 * 2. On success, attempt to download the test image.
 * 3. If download is successful, display the image.
 * 4. Otherwise, display a failure message.
 */
class TrmnlTestActivity final : public ActivityWithSubactivity {
 public:
  using OnFinishCallback = std::function<void()>;

  explicit TrmnlTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OnFinishCallback onFinish);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State {
    WIFI_SELECTION,
    DOWNLOADING,
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

  // Helper to render the downloaded BMP image
  static void drawBmpFromFile(GfxRenderer& renderer, const char* filename, int x, int y);
};