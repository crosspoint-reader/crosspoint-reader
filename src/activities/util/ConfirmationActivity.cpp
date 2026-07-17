#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  // Text sits in the upper part of the screen so the confirmation popup
  // (centered) doesn't cover it.
  startY = renderer.getScreenHeight() / 6;

  const char* options[] = {I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM)};
  confirmPopup.show(safeHeading.c_str(), options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
