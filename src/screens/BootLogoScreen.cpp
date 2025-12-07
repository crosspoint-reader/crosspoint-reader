#include "BootLogoScreen.h"

#include <EpdRenderer.h>

#include "images/CrossLarge.h"

void BootLogoScreen::onEnter() {
  const auto pageWidth = renderer.getPageWidth();
  const auto pageHeight = renderer.getPageHeight();

  renderer.clearScreen();
  // Location for images is from top right in landscape orientation
  renderer.drawImage(CrossLarge, (pageHeight - 128) / 2, (pageWidth - 128) / 2, 128, 128);
  const int width = renderer.getUiTextWidth("CrossPoint", BOLD);
  renderer.drawUiText((pageWidth - width)/ 2, pageHeight / 2 + 100, "CrossPoint", true, BOLD);
  const int bootingWidth = renderer.getSmallTextWidth("BOOTING");
  renderer.drawSmallText((pageWidth - bootingWidth) / 2, pageHeight / 2 + 125, "BOOTING");
  renderer.flushDisplay();
}
