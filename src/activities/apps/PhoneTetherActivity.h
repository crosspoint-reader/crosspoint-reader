#pragma once
#include <cstdint>

#include "activities/Activity.h"

class PhoneTetherActivity final : public Activity {
 public:
  explicit PhoneTetherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PhoneTether", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == MONITORING || state == ALERT; }
  bool skipLoopDelay() override { return state == MONITORING; }

 private:
  enum State { CONFIG, MONITORING, ALERT };
  State state = CONFIG;

  // Config
  char targetMac[18] = "00:00:00:00:00:00";
  int8_t rssiThreshold = -80;
  int editField = 0;  // 0-5: MAC bytes, 6: threshold
  uint8_t macBytes[6] = {};

  // Monitoring
  bool targetFound = false;
  int8_t currentRssi = -127;
  char targetName[33] = {};
  unsigned long lastSeenTime = 0;
  static constexpr unsigned long LOST_TIMEOUT_MS = 15000;

  // BLE scan state
  bool scanInitialized = false;
  bool needsInit = false;
  unsigned long lastScanTime = 0;
  char bleError[24] = {};  // non-empty when BLE init failed

  // RSSI history for graph
  static constexpr int HISTORY_SIZE = 30;
  int8_t rssiHistory[HISTORY_SIZE] = {};
  int historyIndex = 0;
  int historyCount = 0;

  void loadConfig();
  void saveConfig();
  void parseMacBytes();
  void buildMacString();
  void startBleScan();
  void processScanResults();
  void pushRssiHistory(int8_t rssi);

  void renderConfig() const;
  void renderMonitoring() const;
  void renderAlert() const;
};
