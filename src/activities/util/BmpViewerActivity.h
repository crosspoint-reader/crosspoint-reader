#pragma once

#include <functional>
#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string filePath;
  bool renderBmpImage(bool showControls = true);
  bool renderDecodedImage(bool showControls = true);
  void renderError(const char* message);
  void setAsSleepScreen();
};