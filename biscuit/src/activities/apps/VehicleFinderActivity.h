#pragma once
#include "activities/Activity.h"

class VehicleFinderActivity final : public Activity {
 public:
  explicit VehicleFinderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("VehicleFinder", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == FINDING; }

 private:
  enum State { IDLE, SAVED, FINDING };

  struct Fingerprint {
    uint8_t bssid[6];
    int8_t rssi;
  };

  State state = IDLE;
  Fingerprint savedAps[10];
  int savedApCount = 0;
  bool hasSavedSpot = false;
  float currentMatch = 0.0f;
  float prevMatch = 0.0f;
  unsigned long lastScan = 0;
  int trendHistory[5] = {};
  int trendHead = 0;
  int trendCount = 0;

  static constexpr const char* SAVE_PATH = "/biscuit/parking.dat";

  void saveSpot();
  void loadSpot();
  float calcMatch(const Fingerprint* current, int count) const;
  void doScan();
  void renderIdle() const;
  void renderSaved() const;
  void renderFinding() const;
};
