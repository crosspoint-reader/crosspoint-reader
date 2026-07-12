#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class WardrivingActivity final : public Activity {
 public:
  explicit WardrivingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wardriving", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  struct SeenNetwork {
    std::string ssid;
    std::string bssid;
    int32_t rssi;
    uint8_t channel;
    uint8_t encType;
    unsigned long firstSeen;
    unsigned long lastSeen;
  };

  bool logging = false;
  std::vector<SeenNetwork> networks;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int totalNewThisScan = 0;
  int scanCount = 0;
  unsigned long startTime = 0;
  unsigned long lastScanTime = 0;
  static constexpr unsigned long SCAN_INTERVAL_MS = 10000;
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;
  static constexpr int MAX_NETWORKS = 500;

  std::string filename;

  void startLogging();
  void stopLogging();
  void processScanResults();
  void appendNetworkToCsv(const SeenNetwork& net);
  static const char* encryptionString(uint8_t type);
};
