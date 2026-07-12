#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BleProximityActivity final : public Activity {
 public:
  explicit BleProximityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleProximity", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  struct BleTarget {
    std::string name;
    std::string mac;
    int32_t rssi;
    unsigned long lastSeen;
    bool active;
  };

  std::vector<BleTarget> devices;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool scanning = false;
  bool scanInitialized = false;
  bool needsInit = false;
  unsigned long lastScanTime = 0;
  static constexpr unsigned long STALE_TIMEOUT_MS = 15000;
  static constexpr int MAX_DEVICES = 50;

  void startBleScan();
  void processScanResults();
  void pruneStale();
};
