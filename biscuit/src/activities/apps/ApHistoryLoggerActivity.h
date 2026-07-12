#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ApHistoryLoggerActivity final : public Activity {
 public:
  explicit ApHistoryLoggerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ApHistoryLogger", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == LOGGING; }

 private:
  enum State { CONFIG, LOGGING, STATS };

  State state = CONFIG;
  int intervalIndex = 0;  // index into INTERVALS[]
  ButtonNavigator buttonNavigator;

  // Logging runtime
  unsigned long logStartMs = 0;
  unsigned long lastScanMs = 0;
  int scanCount = 0;
  uint32_t fileSize = 0;

  // Unique BSSID tracking (as 6-byte packed uint64, lower 48 bits)
  std::vector<uint64_t> seenBssids;  // max 200

  static constexpr unsigned long INTERVALS[] = {
      60000UL,       // 1 min
      300000UL,      // 5 min
      600000UL,      // 10 min
      1800000UL,     // 30 min
  };
  static constexpr int NUM_INTERVALS = 4;
  static constexpr const char* INTERVAL_LABELS[] = {"1 min", "5 min", "10 min", "30 min"};
  static constexpr int MAX_SEEN = 200;
  static constexpr const char* LOG_PATH = "/biscuit/logs/ap_history.csv";

  void doScan();
  static uint64_t bssidToU64(const uint8_t* bssid);
};
