#pragma once
#include <string>

#include "../Activity.h"

class Bitmap;

void invalidateSleepImageCache();
int validateAndCountSleepImages();

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderImageSleepScreen(const std::string& imagePath) const;
  void renderTransparentSleepScreen() const;

  void drawLockIcon(int cx, int cy) const;

  static constexpr const char* SLEEP_CACHE_PATH = "/.crosspoint/sleep_cache.txt";
};
