#pragma once

#include <functional>
#include <string>

#include "activities/ActivityWithSubactivity.h"

/**
 * Activity for using the CrossPoint as a page turner for KOReader.
 *
 * Sends HTTP GET requests to KOReader's HTTP inspector API to turn pages.
 * The IP:port is configured in KOReader settings; this activity reads it
 * from settings, connects to WiFi, and uses the side buttons to send
 * page forward/back commands.
 *
 * Flow:
 * 1. Connect to WiFi
 * 2. Use side buttons to turn pages on the remote device
 */
class KOReaderPageTurnerActivity final : public ActivityWithSubactivity {
 public:
  explicit KOReaderPageTurnerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                      std::function<void()> onGoBack)
      : ActivityWithSubactivity("KOReaderPageTurner", renderer, mappedInput), onGoBack(std::move(onGoBack)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return state == ACTIVE; }

 private:
  enum State {
    WIFI_SELECTION,
    ACTIVE,
  };

  State state = WIFI_SELECTION;
  std::function<void()> onGoBack;

  std::string deviceAddress;
  std::string errorMessage;

  void onWifiSelectionComplete(bool connected);
  bool sendPageTurn(int direction);
};
