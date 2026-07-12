#pragma once

#include <BLEDevice.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BleBeaconActivity final : public Activity {
 public:
  explicit BleBeaconActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleBeacon", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { MODE_SELECT, SPAMMING };
  enum SpamMode {
    APPLE_APPLEJUICE,
    APPLE_SOURAPPLE,
    SAMSUNG,
    GOOGLE_FAST_PAIR,
    WINDOWS_SWIFTPAIR,
    SPAM_ALL,
    STOP
  };

  static constexpr int MODE_COUNT = 7;
  static constexpr const char* MODE_NAMES[] = {
      "Apple (AppleJuice)", "Apple (SourApple)", "Samsung",
      "Google Fast Pair",   "Windows SwiftPair", "Spam All",
      "Stop"};

  State state = MODE_SELECT;
  SpamMode activeMode = APPLE_APPLEJUICE;
  int modeIndex = 0;
  ButtonNavigator buttonNavigator;

  uint32_t packetsSent = 0;
  unsigned long startTime = 0;
  unsigned long lastCycleTime = 0;
  int deviceTypeIndex = 0;
  int platformIndex = 0;

  BLEAdvertising* pAdvertising = nullptr;

  void startSpam(SpamMode mode);
  void stopSpam();
  void sendAppleJuicePacket();
  void sendSourApplePacket();
  void sendSamsungPacket();
  void sendGoogleFastPairPacket();
  void sendWindowsSwiftPairPacket();
  void cycleSpamAll();

  static void randomizeBleAddress();
};
