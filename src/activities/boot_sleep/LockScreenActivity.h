#pragma once

#include <functional>

#include "../Activity.h"

class LockScreenActivity final : public Activity {
 public:
  explicit LockScreenActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::function<void()> onUnlocked)
      : Activity("LockScreen", renderer, mappedInput), onUnlocked(std::move(onUnlocked)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::function<void()> onUnlocked;
  uint8_t inputBuffer[6] = {};
  uint8_t inputLength = 0;
  unsigned long errorUntil = 0;
  bool unlockPending = false;
};
