#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PerimeterWatchActivity final : public Activity {
 public:
  explicit PerimeterWatchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PerimeterWatch", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == WATCHING; }

 private:
  enum State { SETUP, WATCHING, REPORT };

  struct Mac {
    uint8_t bytes[6];
  };

  struct Intrusion {
    uint8_t mac[6];
    int32_t rssi;
    unsigned long timestamp;
  };

  State state = SETUP;
  std::vector<Mac> baseline;
  std::vector<Intrusion> intrusions;
  int reportIndex = 0;
  ButtonNavigator buttonNavigator;

  unsigned long watchStartMs = 0;
  unsigned long lastScanMs = 0;
  static constexpr unsigned long SCAN_INTERVAL_MS = 60000UL;
  static constexpr int MAX_BASELINE = 100;
  static constexpr int MAX_INTRUSIONS = 50;

  void doBaselineScan();
  void doWatchScan();
  bool macInBaseline(const uint8_t* mac) const;
  void exportCsv();
  static void macToStr(const uint8_t* mac, char* buf, size_t bufLen);
};
