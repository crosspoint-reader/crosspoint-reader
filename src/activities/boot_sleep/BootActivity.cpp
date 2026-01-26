#include "BootActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"
#include "images/CrossLarge.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  Serial.println("[Boot] clearScreen done");
  renderer.drawImage(CrossLarge, (pageWidth + 128) / 2, (pageHeight - 128) / 2, 128, 128);
  Serial.println("[Boot] drawImage done");
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  Serial.println("[Boot] CrossPoint text done");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "BOOTING");
  Serial.println("[Boot] BOOTING text done");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  Serial.println("[Boot] Version text done");
  renderer.displayBuffer();
  Serial.println("[Boot] displayBuffer done");
}
