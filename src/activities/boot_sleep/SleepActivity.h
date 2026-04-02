#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderLogoSleepScreen() const;
  bool renderCustomSleepScreen() const;
  bool renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool allowCrop) const;
  void renderBlankSleepScreen() const;
};
