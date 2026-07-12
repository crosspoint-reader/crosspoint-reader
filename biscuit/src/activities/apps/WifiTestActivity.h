#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class WifiTestActivity final : public Activity {
 public:
  explicit WifiTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WifiTest", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == DEAUTHING; }

 private:
  enum State { SCANNING, SELECT_TARGET, DEAUTHING, NOT_SUPPORTED };

  struct AP {
    std::string ssid;
    uint8_t bssid[6];
    int32_t rssi;
    uint8_t channel;
  };

  State state = SCANNING;
  std::vector<AP> aps;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int targetIndex = 0;
  uint32_t packetsSent = 0;
  unsigned long startTime = 0;
  unsigned long lastSendTime = 0;
  bool rawTxAvailable = false;

  void startScan();
  void processScanResults();
  bool sendDeauthFrame(const uint8_t* bssid, uint8_t channel);
};
