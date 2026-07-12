#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PingActivity final : public Activity {
 public:
  explicit PingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Ping", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State { TEXT_INPUT, PINGING, SUMMARY };

  State state = TEXT_INPUT;
  std::string targetHost;

  struct PingResult {
    bool success = false;
    unsigned long rttMs = 0;
  };

  std::vector<PingResult> results;
  static constexpr int MAX_DISPLAY_RESULTS = 5;
  static constexpr unsigned long PING_INTERVAL_MS = 2000;
  unsigned long lastPingTime = 0;
  int totalSent = 0;
  int totalSuccess = 0;
  unsigned long minRtt = 0;
  unsigned long maxRtt = 0;
  unsigned long totalRtt = 0;

  void doPing();
  void renderPinging() const;
  void renderSummary() const;
};
