#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class PxcViewerActivity final : public Activity {
 public:
  PxcViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void onScreenshotRequest() override;

 private:
  std::string filePath;
  void renderGrayscaleImage();
  void renderPxcToFramebuffer(FsFile& file, uint16_t width, uint16_t height, uint32_t dataOffset);
};
