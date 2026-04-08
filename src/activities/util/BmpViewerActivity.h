#pragma once

#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <I18nKeys.h>

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
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  uint8_t imageDitherMode;
  uint8_t initialImageDitherMode;
  bool imageDitherSettingsDirty;
#endif
  // Per-session toggle: monochrome (1-bit Atkinson, single decode) vs grayscale (4-level dither, multipass).
  // Not persisted — defaults to grayscale every time the viewer opens.
  bool grayscaleDisplay = true;
  // True after a successful BMP render iff the bitmap actually carries greyscale data.
  // Used to gate the BW/Gray toggle in loop() — pure 1-bit BMPs cannot be toggled.
  bool bmpHasGreyscale = false;
  bool renderCurrentImage(bool showControls = true);
  bool renderBmpImage(bool showControls = true);
  bool renderDecodedImage(bool showControls = true);
  void toggleDisplayMode();
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  void cycleDitherMode();
  StrId getCurrentDitherModeLabel() const;
  void saveDitherSettingsIfNeeded();
#endif
  void renderError(const char* message);
  void setAsSleepScreen();
};
