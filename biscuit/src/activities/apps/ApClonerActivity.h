#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ApClonerActivity final : public Activity {
 public:
  explicit ApClonerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ApCloner", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return cloneActive; }

 private:
  enum State { SCANNING, SELECT_AP, CLONE_RUNNING };

  struct AP {
    std::string ssid;
    int32_t rssi;
    uint8_t channel;
    uint8_t encType;
  };

  State state = SCANNING;
  std::vector<AP> aps;
  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;

  bool cloneActive = false;
  std::string clonedSsid;
  uint8_t clonedChannel = 1;
  unsigned long cloneStartTime = 0;
  int connectedClients = 0;
  unsigned long lastUpdateTime = 0;

  void startScan();
  void processScanResults();
  void startClone(int apIndex);
  void stopClone();
  static const char* encryptionString(uint8_t type);
};
