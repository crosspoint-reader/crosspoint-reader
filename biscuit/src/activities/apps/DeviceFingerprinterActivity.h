#pragma once
#include <cstdint>
#include <vector>
#include <esp_wifi.h>
#include <freertos/portmacro.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DeviceFingerprinterActivity final : public Activity {
 public:
  explicit DeviceFingerprinterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeviceFingerprinter", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return promiscuousActive; }

  // Called from promiscuous callback — must be public
  void onProbePacket(const uint8_t* payload, uint16_t len, int rssi);

 private:
  enum State { READY, CAPTURING };
  State state = READY;

  void startCapture();
  struct FingerprintedDevice {
    uint8_t mac[6];
    char estimatedOs[16];
    int probeCount;
    int8_t rssi;
  };

  static constexpr int MAX_DEVICES = 50;

  std::vector<FingerprintedDevice> devices;
  int deviceIndex = 0;
  ButtonNavigator buttonNavigator;

  bool promiscuousActive = false;
  unsigned long lastDisplay = 0;
  static constexpr unsigned long DISPLAY_INTERVAL_MS = 2000UL;

  portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

  static DeviceFingerprinterActivity* activeInstance;
  static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type);

  static const char* estimateOs(const uint8_t* mac, int probeCount);
};
