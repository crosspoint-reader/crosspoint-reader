#include "SleepActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "SD.h"
#include "config.h"
#include "images/CrossLarge.h"

void SleepActivity::onEnter() {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  auto file = SD.open("/sleep.bmp");
  if (file) {
    renderCustomSleepScreen(file);
    return;
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() {
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(CrossLarge, (pageWidth - 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(UI_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Apply white screen if enabled in settings
  if (!SETTINGS.whiteSleepScreen) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}

void SleepActivity::renderCustomSleepScreen(File& file) {
  renderer.clearScreen();
  bool didImageDrawSuccessfully = renderer.drawFullScreenBmp(file);

  if (!didImageDrawSuccessfully) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_FONT_ID, GfxRenderer::getScreenHeight() / 2, "BAD CUSTOM SLEEP SCREEN", true, BOLD);
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}