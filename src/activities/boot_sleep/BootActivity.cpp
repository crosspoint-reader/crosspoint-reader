#include "BootActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);

  // Custom Mod Title
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "Crosspoint-Button-Mod", true, EpdFontFamily::BOLD);

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "BOOTING");

  // Custom Version Number
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, "1.0.0-dev-ztrawhcs.1");

  renderer.displayBuffer();
}
