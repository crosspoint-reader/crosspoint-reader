#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class WifiScannerActivity final : public Activity {
 public:
  explicit WifiScannerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WifiScanner", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { SCANNING, LIST, DETAIL };
  enum ViewMode { LIST_VIEW, SIGNAL_VIEW, CHANNEL_VIEW };
  enum ChannelViewMode { VIEW_SPECTRUM, VIEW_TABLE };

  struct Network {
    std::string ssid;
    std::string bssid;
    int32_t rssi;
    uint8_t channel;
    uint8_t encType;  // wifi_auth_mode_t
  };

  // Channel analyzer state
  struct ChannelInfo {
    int apCount;
    int32_t strongestRssi;
    int32_t avgRssi;
    int64_t rssiSum;
    int interferenceScore;
  };

  State state = SCANNING;
  ViewMode viewMode = LIST_VIEW;
  ChannelViewMode channelViewMode = VIEW_SPECTRUM;

  std::vector<Network> networks;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int detailIndex = 0;
  bool sortBySignal = true;

  // Signal meter state
  int targetIndex = -1;
  int32_t currentRssi = -100;
  int32_t minRssi = 0;
  int32_t maxRssi = -100;
  int32_t avgRssi = -100;
  int sampleCount = 0;
  int64_t rssiSum = 0;
  static constexpr int RSSI_HISTORY_SIZE = 40;
  int32_t rssiHistory[RSSI_HISTORY_SIZE] = {};
  int rssiHistoryIndex = 0;
  int rssiHistoryCount = 0;
  unsigned long lastMeasureTime = 0;
  static constexpr unsigned long MEASURE_INTERVAL_MS = 500;

  // Channel analyzer state
  ChannelInfo channelData[14] = {};  // index 1-13
  int recommendedChannel = 1;

  void startScan();
  void processScanResults();
  void sortNetworks();
  void saveToCsv();
  const char* encryptionString(uint8_t type) const;
  const char* signalBars(int32_t rssi) const;

  void doMeasurement();
  void analyzeChannels();
  int findBestChannel() const;
  void renderSignalView();
  void renderChannelSpectrum();
  void renderChannelTable();
};
