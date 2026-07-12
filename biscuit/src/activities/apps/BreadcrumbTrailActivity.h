#pragma once
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BreadcrumbTrailActivity final : public Activity {
 public:
  explicit BreadcrumbTrailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BreadcrumbTrail", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == RECORDING || state == RETRACING; }

 private:
  enum State { MENU, RECORDING, TRAIL_LIST, RETRACING };

  struct ApSig {
    uint8_t bssid[6];
    int8_t rssi;
  };
  struct Crumb {
    ApSig aps[5];
    int apCount;
    unsigned long timestamp;
  };

  State state = MENU;
  std::vector<Crumb> trail;
  static constexpr int MAX_CRUMBS = 200;
  int retraceIndex = 0;
  float matchScore = 0.0f;
  unsigned long lastCrumb = 0;
  int menuIndex = 0;
  ButtonNavigator buttonNavigator;

  // Trail list state
  std::vector<std::string> trailFiles;
  int trailListIndex = 0;

  static constexpr const char* TRAILS_DIR = "/biscuit/trails";

  void doScan(Crumb& out);
  float jaccardScore(const Crumb& crumb, const ApSig* current, int currentCount) const;
  void saveToDisk(const char* name) const;
  bool loadFromDisk(const char* path);
  void loadTrailList();
  void promptSaveName();

  void renderMenu() const;
  void renderRecording() const;
  void renderTrailList() const;
  void renderRetracing() const;
};
