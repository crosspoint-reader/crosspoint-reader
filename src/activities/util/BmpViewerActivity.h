#pragma once

#include <functional>
#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    std::function<void()> onGoBack);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();

  std::string filePath;
  std::function<void()> onGoBack;
  bool isConfirmingDelete = false;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};