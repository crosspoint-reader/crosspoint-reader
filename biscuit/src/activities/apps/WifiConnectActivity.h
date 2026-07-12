#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct WifiNetworkInfo;

class WifiConnectActivity final : public Activity {
 public:
  explicit WifiConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WifiConnect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { SCANNING, LIST, CONNECTING, CONNECTED };

  struct ApInfo {
    std::string ssid;
    int32_t rssi = 0;
    bool encrypted = false;
    bool saved = false;
  };

  State state = SCANNING;
  std::vector<ApInfo> networks;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;

  std::string selectedSSID;
  std::string enteredPassword;
  std::string connectedIP;
  int connectedRSSI = 0;
  unsigned long connectionStartTime = 0;
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;

  void startScan();
  void processScanResults();
  void selectNetwork(int index);
  void attemptConnection();
  void checkConnectionStatus();

  void renderScanning() const;
  void renderList() const;
  void renderConnecting() const;
  void renderConnected() const;

  static std::string signalBars(int32_t rssi);
};
