#pragma once
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false,
                         bool quiet = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), quiet(quiet) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
  // Suppress the "Entering sleep" popup — used by the clock-peek restore path,
  // which redraws the sleep screen while the user is already looking at it.
  bool quiet = false;
};
