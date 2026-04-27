#pragma once
#include <string>

#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;
  void onScreenshotRequest() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  bool renderPxcSleepScreen(const std::string& path) const;
  void renderBlankSleepScreen() const;

  // Tracks the last factory-LUT render so onScreenshotRequest() can re-render the same image.
  mutable std::string lastGrayscalePath;
  mutable bool lastGrayscaleIsPxc = false;
};
