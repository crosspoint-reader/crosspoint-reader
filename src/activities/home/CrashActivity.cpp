#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }
  HalSystem::clearPanic();

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
