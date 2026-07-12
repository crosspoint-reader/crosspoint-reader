#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <freertos/portmacro.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ProbeSnifferActivity final : public Activity {
 public:
  explicit ProbeSnifferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ProbeSniffer", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return sniffing; }

  void onProbeRequest(const uint8_t* srcMac, const char* ssid, int rssi);

 private:
  enum State { SNIFFING_VIEW, DETAIL };

  struct ProbeEntry {
    uint8_t mac[6];
    std::string ssid;
    int rssi;
    uint32_t count;
    unsigned long lastSeen;
  };

  State state = SNIFFING_VIEW;
  std::vector<ProbeEntry> entries;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int detailIndex = 0;
  bool sniffing = false;
  unsigned long lastUpdateTime = 0;
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;
  unsigned long lastHopTime = 0;
  uint8_t currentChannel = 1;
  static constexpr unsigned long UPDATE_INTERVAL_MS = 2000;
  static constexpr unsigned long HOP_INTERVAL_MS = 500;
  static constexpr int MAX_ENTRIES = 100;

  portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

  void startSniffing();
  void stopSniffing();
  void saveToCsv();
  static std::string macToString(const uint8_t* mac);
};
