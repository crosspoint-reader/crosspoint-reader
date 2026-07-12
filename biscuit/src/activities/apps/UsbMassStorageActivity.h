#pragma once
#include "activities/Activity.h"

class UsbMassStorageActivity final : public Activity {
 public:
  explicit UsbMassStorageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbMassStorage", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == ACTIVE; }

 private:
  enum State { READY, ACTIVE, EJECTED };
  State state = READY;

  // USB MSC interface — MANUAL IMPLEMENTATION
  void initMassStorage();   // TODO: MANUAL — TinyUSB MSC descriptor + SD card sector callbacks
  void deinitMassStorage();  // TODO: MANUAL — restore CDC mode, remount SD for firmware use
};
