#pragma once

#include <string>

#include "../Activity.h"

class Bitmap;

struct BookOverlayInfo {
  std::string title;
  std::string author;
  std::string progressText;
  std::string chapterName;
  std::string progressSuffix;
};

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderBlankSleepScreen() const;
  BookOverlayInfo getBookOverlayInfo(const std::string& bookPath) const;
};
