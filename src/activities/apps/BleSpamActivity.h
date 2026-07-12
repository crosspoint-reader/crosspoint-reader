#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BleSpamActivity final : public Activity {
 public:
  explicit BleSpamActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleSpam", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == BROADCASTING; }
  bool skipLoopDelay() override { return state == BROADCASTING; }

 private:
  enum State { DISCLAIMER, MENU, BROADCASTING };
  enum AdvType {
    APPLE_PROXIMITY,
    ANDROID_FAST_PAIR,
    WINDOWS_SWIFT_PAIR,
    SAMSUNG_BUDS,
    RANDOM_ALL,
    ADV_TYPE_COUNT
  };

  State state = DISCLAIMER;
  AdvType advType = RANDOM_ALL;
  int menuIndex = 0;
  ButtonNavigator buttonNavigator;

  // Broadcasting state
  unsigned long lastAdvTime = 0;
  int sentCount = 0;
  unsigned long startTime = 0;
  int currentRandomType = 0;
  static constexpr unsigned long ADV_INTERVAL_MS = 100;

  // Display throttle
  unsigned long lastDisplayMs = 0;
  static constexpr unsigned long DISPLAY_INTERVAL_MS = 500;

  void startBroadcasting();
  void stopBroadcasting();
  void sendNextAdvertisement();

  // Payload generators
  void sendAppleProximityAdv();
  void sendAndroidFastPairAdv();
  void sendWindowsSwiftPairAdv();
  void sendSamsungBudsAdv();

  static const char* advTypeName(AdvType type);
};
