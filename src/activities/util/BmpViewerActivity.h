#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderImage();
  bool saveJpegSleepCover();

  std::atomic<unsigned long> paintedAtMs{0};
  std::atomic<bool> imagePainted{false};
  bool settledRedrawDone = false;
  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};
