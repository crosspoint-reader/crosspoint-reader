#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class VendorOuiActivity final : public Activity {
 public:
  explicit VendorOuiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("VendorOui", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MENU, SCANNING, SCAN_LIST, RESULT };

  struct ScannedAp {
    uint8_t bssid[6];
    int32_t rssi;
    std::string ssid;
  };

  State state = MENU;
  int menuIndex = 0;       // 0=Enter MAC, 1=WiFi Scan
  int scanIndex = 0;
  ButtonNavigator buttonNavigator;

  std::string inputMac;    // raw from keyboard
  std::string displayMac;  // formatted XX:XX:XX:XX:XX:XX
  std::string vendorResult;
  bool lookupDone = false;

  std::vector<ScannedAp> scanResults;

  void startKeyboardEntry();
  void startWifiScan();
  void lookupOui(const uint8_t* ouiBytes);
  static void formatMac(const uint8_t* bytes, char* buf, size_t len);
  static bool parseHexMac(const char* str, uint8_t* out6);
};
