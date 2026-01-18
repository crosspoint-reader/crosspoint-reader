#pragma once

#include "../Activity.h"

/**
 * CalendarActivity - Automated calendar image fetch and display
 *
 * This activity is triggered on timer wake (not power button wake).
 * It connects to WiFi, fetches a BMP image from a configured URL,
 * saves it as the sleep screen, and returns to deep sleep.
 *
 * Flow:
 * 1. Load saved WiFi credentials
 * 2. Connect to WiFi (timeout: 30s)
 * 3. HTTP GET image from configured URL (timeout: 60s)
 * 4. Save image to /sleep.bmp on SD card
 * 5. Render sleep screen
 * 6. Schedule timer wake for next refresh
 * 7. Enter deep sleep
 */

enum class CalendarState { INIT, CONNECTING_WIFI, FETCHING_IMAGE, RENDERING, ERROR };

class CalendarActivity final : public Activity {
 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  CalendarState state = CalendarState::INIT;
  unsigned long stateStartTime = 0;
  String errorMessage;

  void startWifiConnection();
  bool checkWifiConnection();
  bool fetchAndSaveImage();
  void handleError(const char* message);
  void renderStatus(const char* status);

  static constexpr unsigned long WIFI_TIMEOUT_MS = 30000;
  static constexpr unsigned long HTTP_TIMEOUT_MS = 60000;
};
