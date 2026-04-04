#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_DISPLAY_QR), nullptr);

  const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int truncNoticeHeight = 16;
  const int availableHeight = contentRect.height - startY - metrics.verticalSpacing - truncNoticeHeight;

  const Rect qrBounds(contentRect.x + metrics.contentSidePadding, startY,
                      contentRect.width - metrics.contentSidePadding * 2, availableHeight);
  const bool truncated = QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  if (truncated) {
    renderer.drawCenteredText(SMALL_FONT_ID, startY + availableHeight + 2, "...", true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
