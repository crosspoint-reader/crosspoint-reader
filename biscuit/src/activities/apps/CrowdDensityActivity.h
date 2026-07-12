#pragma once
#include <cstdint>
#include <esp_wifi.h>
#include <freertos/portmacro.h>

#include "activities/Activity.h"

class CrowdDensityActivity final : public Activity {
 public:
  explicit CrowdDensityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CrowdDensity", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return promiscuousActive; }

  // Called from promiscuous callback — must be public
  void addMac(const uint8_t* mac);

 private:
  enum State { READY, CAPTURING };
  State state = READY;

  void startCapture();
  static constexpr int HISTORY_SIZE = 60;   // 30 minutes at 30 s intervals
  static constexpr int MAX_MACS = 256;
  static constexpr unsigned long SAMPLE_INTERVAL_MS = 30000UL;
  static constexpr unsigned long DISPLAY_INTERVAL_MS = 2000UL;

  int history[HISTORY_SIZE] = {};
  int historyHead = 0;
  int historyCount = 0;
  int currentCount = 0;

  unsigned long lastSample = 0;
  unsigned long lastDisplay = 0;
  unsigned long windowStart = 0;

  uint8_t seenMacs[MAX_MACS][6] = {};
  int seenMacCount = 0;

  bool promiscuousActive = false;

  portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

  static CrowdDensityActivity* activeInstance;
  static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type);
};
