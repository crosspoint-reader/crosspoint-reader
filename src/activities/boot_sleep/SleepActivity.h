#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class SleepOverlay;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

  // True if a sleep overlay image exists on the SD card (root file or overlay directory).
  static bool hasSleepOverlayImage();

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderCurrentScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderWithOverlay(const Bitmap* background) const;

  bool fromTimeout = false;
  // Set by onEnter() while rendering when the sleep screen overlay setting is on.
  SleepOverlay* overlay = nullptr;
};
