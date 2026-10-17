#pragma once

#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <I18nKeys.h>

#include <functional>
#include <string>

#include "../Activity.h"
#include "ConfirmationActivity.h"
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
  // Per-session, like grayscaleDisplay below. It used to be held in
  // CrossPointSettings::imageDithering and "saved" — but that field had no
  // registry row and no serializer line, so every save wrote nothing and every
  // boot reset it. Keeping it here says what it has always actually done.
  uint8_t imageDitherMode;
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
#endif
  void renderError(const char* message);
  void confirmSetAsSleepScreen();
  void setAsSleepScreen();

 public:
  // Sets a BMP file as the sleep screen without needing an open viewer instance.
  // Returns true on success.  Only works for .bmp files; for JPG/PNG open BmpViewerActivity.
  static bool setBmpFileAsSleepScreen(const std::string& filePath);

  // Asked before an image replaces /sleep.bmp, which keeps no copy of the image it held.
  static ConfirmationActivity::Question sleepScreenQuestion(const std::string& imagePath);
};
