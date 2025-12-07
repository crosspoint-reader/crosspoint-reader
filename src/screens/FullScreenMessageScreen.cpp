#include "FullScreenMessageScreen.h"

#include <EpdRenderer.h>

void FullScreenMessageScreen::onEnter() {
  const auto width = renderer.getUiTextWidth(text.c_str(), style);
  const auto height = renderer.getLineHeight();
  const auto left = (renderer.getPageWidth() - width) / 2;
  const auto top = (renderer.getPageHeight() - height) / 2;

  renderer.clearScreen();
  renderer.drawUiText(left, top, text.c_str(), true, style);
  renderer.flushDisplay(refreshMode);
}
