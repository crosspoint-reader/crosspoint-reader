#pragma once

#include "activities/Activity.h"

/**
 * BookFusion account linking via OAuth 2.0 Device Code Flow.
 *
 * Displays a short code and URL on the E-ink screen.
 * The user visits the URL on their phone and enters the code.
 * The activity polls the BookFusion server in loop() via millis() until
 * authorised, expired, or cancelled — no FreeRTOS task needed.
 */
class BookFusionAuthActivity final : public Activity {
 public:
  explicit BookFusionAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CONNECTING || state == REQUESTING_CODE || state == WAITING_FOR_USER || state == POLLING;
  }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    REQUESTING_CODE,
    WAITING_FOR_USER,
    POLLING,
    SUCCESS,
    EXPIRED,
    DENIED,
    FAILED
  };

  State state = WIFI_SELECTION;

  // Device code response data (fixed-size buffers — no heap allocation)
  char deviceCode[256] = {};
  char userCode[16] = {};
  char verificationUri[128] = {};
  int pollIntervalSec = 5;
  unsigned long pollExpireAt = 0;  // millis() deadline
  unsigned long nextPollAt = 0;    // millis() of next poll attempt
  unsigned long lastTimerRefresh = 0;
  int networkRetries = 0;

  void onWifiSelectionComplete(bool success);
  void requestCode();
  void doPoll();
};
