#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SweepActivity final : public Activity {
 public:
  explicit SweepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sweep", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { READY, SCANNING, RESULTS };
  State state = READY;
  int scanPhase = 0;  // 0=wifi cameras, 1=wifi karma, 2=ble trackers+skimmers

  struct Finding { char description[80]; int severity; };  // severity: 0=info, 1=warning, 2=critical
  std::vector<Finding> findings;
  int findingIndex = 0;
  ButtonNavigator buttonNavigator;

  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;

  int trackersFound = 0;
  int suspiciousCams = 0;
  int rogueAps = 0;
  int skimmers = 0;

  void startSweep();
  void scanWifiCameras();
  void scanWifiKarma();
  void scanBleThreats();
  void addFinding(const char* desc, int severity);

  void renderReady() const;
  void renderScanning() const;
  void renderResults() const;
};
