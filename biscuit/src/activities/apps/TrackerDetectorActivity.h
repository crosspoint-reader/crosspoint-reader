#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TrackerDetectorActivity final : public Activity {
 public:
  explicit TrackerDetectorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TrackerDetector", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return monitoring; }

 private:
  enum TrackerType : uint8_t { APPLE_AIRTAG, APPLE_FINDMY, SAMSUNG_SMARTTAG, TILE, GOOGLE_FINDER, UNKNOWN_TRACKER };

  struct TrackedDevice {
    std::string mac;
    TrackerType type;
    int8_t rssi;
    uint8_t seenCount;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool flagged;
  };

  enum State { IDLE, MONITORING, ALERT };

  State state = IDLE;
  bool monitoring = false;
  bool scanInitialized = false;
  bool needsBleInit = false;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  std::vector<TrackedDevice> devices;
  static constexpr int MAX_TRACKED = 50;

  unsigned long lastScanTime = 0;
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;
  static constexpr unsigned long SCAN_INTERVAL_MS = 30000;
  static constexpr unsigned long SCAN_DURATION_S = 3;
  uint16_t scanCycleCount = 0;

  static constexpr uint8_t FOLLOW_THRESHOLD = 3;
  static constexpr unsigned long FOLLOW_TIME_MS = 1800000;
  static constexpr unsigned long STALE_TIMEOUT_MS = 3600000;
  int alertCount = 0;

  void startMonitoring();
  void stopMonitoring();
  void runScan();
  void processScanResults();
  void checkForFollowers();
  void pruneStale();

  static TrackerType identifyTracker(const uint8_t* mfData, size_t mfLen, const std::string& serviceUuids);
  static const char* trackerTypeName(TrackerType type);

  void renderIdle() const;
  void renderMonitoring() const;
  void renderAlert() const;
};
