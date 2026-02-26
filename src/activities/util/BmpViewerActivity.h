#pragma once

#include <functional>
#include <string>

#include "../ActivityWithSubactivity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public ActivityWithSubactivity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    std::function<void()> onGoBack);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doDelete();
  void doSetSleepCover();

  std::string filePath;
  std::function<void()> onGoBack;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};