#pragma once
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DeauthDetectorActivity final : public Activity {
 public:
  explicit DeauthDetectorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeauthDetector", renderer, mappedInput) {}

  ~DeauthDetectorActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == MONITORING; }

  // Called from promiscuous callback — must be FAST, ISR-safe, no heap
  void onPacket(const uint8_t* data, uint16_t len, int rssi);

 private:
  enum State { IDLE, MONITORING, ALERT_SPIKE };
  State state = IDLE;

  // Stats (updated from callback via spinlock)
  portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint32_t deauthCount = 0;
  volatile uint32_t disassocCount = 0;
  volatile uint32_t totalFrames = 0;
  volatile uint32_t intervalDeauth = 0;  // deauths in current interval

  // Recent events ring buffer (fixed size, no heap alloc in callback)
  struct DeauthEvent {
    uint8_t srcMac[6];
    uint8_t dstMac[6];
    uint8_t type;   // 0 = deauth, 1 = disassoc
    int8_t rssi;
  };
  static constexpr int EVENT_LOG_SIZE = 20;
  DeauthEvent eventLog[EVENT_LOG_SIZE]{};
  volatile int eventLogHead = 0;
  volatile int eventLogCount = 0;

  // Spike alert: count in the interval that triggered the alert
  uint32_t spikeCount = 0;

  // Channel
  uint8_t currentChannel = 1;
  bool autoHop = true;

  // Timing
  unsigned long lastUpdateTime = 0;
  unsigned long lastHopTime = 0;
  static constexpr unsigned long UPDATE_INTERVAL_MS = 2000;
  static constexpr unsigned long HOP_INTERVAL_MS = 400;

  // Alert threshold
  static constexpr int SPIKE_THRESHOLD = 5;

  // Event log scroll
  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;

  void startMonitor();
  void stopMonitor();
};
