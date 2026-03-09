#include "TodoFallbackActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

void TodoFallbackActivity::onEnter() {
  Activity::onEnter();
  render();
}

void TodoFallbackActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onBack != nullptr) onBack(onBackCtx);
  }
}

void TodoFallbackActivity::render() const {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int cardWidth = pageWidth / 2;
  const int cardHeight = pageHeight / 2;
  const int cardX = (pageWidth - cardWidth) / 2;
  const int cardY = 40;

  renderer.drawRect(cardX, cardY, cardWidth, cardHeight);

  const int bookmarkWidth = cardWidth / 8;
  const int bookmarkHeight = cardHeight / 5;
  const int bookmarkX = cardX + cardWidth - bookmarkWidth - 10;
  const int bookmarkY = cardY + 5;
  const int notchDepth = bookmarkHeight / 3;
  const int centerX = bookmarkX + bookmarkWidth / 2;

  const int xPoints[5] = {bookmarkX, bookmarkX + bookmarkWidth, bookmarkX + bookmarkWidth, centerX, bookmarkX};
  const int yPoints[5] = {bookmarkY, bookmarkY, bookmarkY + bookmarkHeight, bookmarkY + bookmarkHeight - notchDepth,
                          bookmarkY + bookmarkHeight};
  renderer.fillPolygon(xPoints, yPoints, 5, true);

  const int titleY = cardY + 30;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, "Daily TODO", true, EpdFontFamily::BOLD);

  if (!dateText.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, titleY + 30, dateText.c_str());
  }

  const auto labels = mappedInput.mapLabels("« Back", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
