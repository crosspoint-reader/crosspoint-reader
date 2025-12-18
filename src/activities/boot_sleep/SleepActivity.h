#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, InputManager& inputManager) : Activity(renderer, inputManager) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen(const Bitmap& bitmap) const;
};
