#pragma once
#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SignalTriangulationActivity final : public Activity {
 public:
  explicit SignalTriangulationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SignalTriangulation", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return scanning; }

 private:
  enum State { SELECT_AP, TAKE_READINGS, RESULT };

  struct VisibleAp {
    char ssid[33];
    char bssid[18];
    int8_t rssi;
    uint8_t channel;
  };

  struct Reading {
    int8_t rssi;
    bool taken;
  };

  static constexpr int MAX_APS = 30;
  static constexpr int SAMPLES_PER_READING = 3;

  State state = SELECT_AP;

  // SELECT_AP state
  VisibleAp apList[MAX_APS];
  int apCount = 0;
  int apIndex = 0;
  bool initialScanDone = false;
  bool initialScanning = false;

  // Selected target
  char targetBssid[18] = {};
  char targetSsid[33] = {};

  // TAKE_READINGS state
  Reading readings[3] = {};
  int currentReading = 0;
  bool scanning = false;
  int scanSamples = 0;
  int32_t rssiAccumulator = 0;

  ButtonNavigator buttonNavigator;

  void startInitialScan();
  void processInitialScan();
  void startReadingScan();
  void processReadingScan();
};
