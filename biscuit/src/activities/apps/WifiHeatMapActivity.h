#pragma once
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class WifiHeatMapActivity final : public Activity {
 public:
  explicit WifiHeatMapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WifiHeatMap", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == LOGGING; }

 private:
  enum State { IDLE, LOGGING, SUMMARY };

  struct ApReading {
    char bssid[18];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
  };

  static constexpr int MAX_CURRENT = 30;
  static constexpr int MAX_UNIQUE = 100;
  static constexpr int MAX_DATA_POINTS = 10000;
  static constexpr unsigned long SCAN_INTERVAL_MS = 5000;

  State state = IDLE;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  ApReading currentReadings[MAX_CURRENT];
  int currentCount = 0;

  char seenBssids[MAX_UNIQUE][18];
  int seenCount = 0;

  int totalDataPoints = 0;
  int sampleCount = 0;

  char filename[64];
  unsigned long startTime = 0;
  unsigned long lastScanTime = 0;
  bool headerWriteFailed = false;
  bool maxPointsReached = false;

  void startLogging();
  void stopLogging();
  void processScanResults();
  bool isBssidSeen(const char* bssid) const;
};
