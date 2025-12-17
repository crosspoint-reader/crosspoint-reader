#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "config.h"

void FullScreenMessageActivity::onEnter() {
  const auto height = renderer.getLineHeight(UI_FONT_ID);
  const auto top = (GfxRenderer::getScreenHeight() - height) / 2;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_FONT_ID, top, text.c_str(), true, style);
  renderer.displayBuffer(refreshMode);
}
