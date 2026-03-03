#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  auto lines = renderer.wrappedText(UI_10_FONT_ID, text.c_str(), renderer.getScreenWidth() - 48, 5, style);
  if (lines.empty()) {
    lines.emplace_back(text);
  }
  const int totalHeight = static_cast<int>(lines.size()) * (lineHeight + 4);
  int top = (renderer.getScreenHeight() - totalHeight) / 2;
  if (top < 40) {
    top = 40;
  }

  renderer.clearScreen();
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, line.c_str(), true, style);
    top += lineHeight + 4;
  }
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  if (!onDismiss) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onDismiss();
  }
}
