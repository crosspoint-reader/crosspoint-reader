#pragma once
#include "../Activity.h"

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, InputManager& inputManager) : Activity(renderer, inputManager) {}
  void onEnter() override;
};
