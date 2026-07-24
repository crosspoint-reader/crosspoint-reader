#include "SafeBootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void SafeBootActivity::onEnter() {
  Activity::onEnter();
  requestUpdateAndWait();
}

void SafeBootActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void SafeBootActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int x = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SAFE_BOOT_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const char* description = manual_ ? tr(STR_SAFE_BOOT_MANUAL) : tr(STR_SAFE_BOOT_AUTOMATIC);
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, description, contentWidth, 10)) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  y += metrics.verticalSpacing * 2;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_SAFE_BOOT_READ_ONLY), contentWidth, 10)) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONTINUE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
