#pragma once
#include "../Activity.h"
#include "SD.h"

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, InputManager& inputManager) : Activity(renderer, inputManager) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen();
  void renderCustomSleepScreen(File& file);
};
