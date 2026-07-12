#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BeaconTestActivity final : public Activity {
 public:
  explicit BeaconTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BeaconTest", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { MODE_SELECT, RUNNING };
  enum Mode { RANDOM, CUSTOM, RICKROLL, FUNNY };

  State state = MODE_SELECT;
  Mode mode = RANDOM;
  int modeIndex = 0;
  ButtonNavigator buttonNavigator;

  std::vector<std::string> ssids;
  int currentSsidIndex = 0;
  int cycleCount = 0;
  unsigned long lastCycleTime = 0;
  static constexpr unsigned long CYCLE_INTERVAL_MS = 2000;

  bool apActive = false;

  void loadSsidsForMode();
  void loadCustomSsids();
  std::string generateRandomSsid();
  void startAP(const std::string& ssid);
  void stopAP();
  void cycleNext();
};
