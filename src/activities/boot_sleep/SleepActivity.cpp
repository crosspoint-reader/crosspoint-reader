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
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderCustomSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
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

void SleepActivity::renderCustomSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = GfxRenderer::getScreenWidth();
  const auto pageHeight = GfxRenderer::getScreenHeight();

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      x = 0;
      y = (pageHeight - pageWidth / ratio) / 2;
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      x = (pageWidth - pageHeight * ratio) / 2;
      y = 0;
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  if (bitmap.hasGreyscale()) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}
