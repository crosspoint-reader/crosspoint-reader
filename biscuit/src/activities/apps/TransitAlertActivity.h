#pragma once
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TransitAlertActivity final : public Activity {
 public:
  explicit TransitAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TransitAlert", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == MONITORING; }

 private:
  enum State { SETUP, SELECT_TARGET, MONITORING, ALERT };

  struct Stop {
    char name[32];
    uint8_t bssids[5][6];
    int8_t rssis[5];
    int apCount;
  };

  State state = SETUP;
  std::vector<Stop> stops;
  int targetStop = 0;
  float matchScore = 0.0f;
  static constexpr float ALERT_THRESHOLD = 60.0f;
  int stopIndex = 0;
  int setupMenuIndex = 0;
  ButtonNavigator buttonNavigator;
  unsigned long lastScan = 0;

  static constexpr const char* STOPS_PATH = "/biscuit/stops.dat";

  void loadStops();
  void saveStops();
  float calcMatch(int stopIdx, const uint8_t bssids[][6], int count) const;
  void doScan();
  void promptStopName();

  void renderSetup() const;
  void renderSelectTarget() const;
  void renderMonitoring() const;
  void renderAlert() const;
};
