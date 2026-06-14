#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body, bool overlay)
    : Activity("Confirmation", renderer, mappedInput), overlay(overlay), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  // In overlay mode the dialog box is inset from the edges, so reserve padding for it.
  const int boxPadding = 20;
  const int maxWidth =
      overlay ? (renderer.getScreenWidth() - (margin + boxPadding) * 2) : (renderer.getScreenWidth() - margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  if (overlay) {
    boxW = renderer.getScreenWidth() - margin * 2;
    boxH = totalHeight + boxPadding * 2;
    boxX = (renderer.getScreenWidth() - boxW) / 2;
    boxY = (renderer.getScreenHeight() - boxH) / 2;
    startY = boxY + boxPadding;
  } else {
    startY = (renderer.getScreenHeight() - totalHeight) / 2;
  }

  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  if (overlay) {
    // Composite a centred box over the caller's screen (no clearScreen).
    constexpr int haloWidth = 3;
    constexpr int cornerRadius = 10;
    renderer.fillRoundedRect(boxX - haloWidth, boxY - haloWidth, boxW + haloWidth * 2, boxH + haloWidth * 2,
                             cornerRadius + haloWidth, Color::White);
    renderer.fillRoundedRect(boxX, boxY, boxW, boxH, cornerRadius, Color::White);
    renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 2, cornerRadius, true);
  } else {
    renderer.clearScreen();
  }

  int currentY = startY;
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // In overlay mode, wipe the launching activity's bottom chrome (its button hints + path line)
  // first, otherwise those labels show through next to this dialog's own Cancel/Confirm hints.
  if (overlay) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int chromeH = metrics.buttonHintsHeight + metrics.verticalSpacing * 2 + renderer.getLineHeight(SMALL_FONT_ID);
    renderer.fillRect(0, renderer.getScreenHeight() - chromeH, renderer.getScreenWidth(), chromeH, false);
  }

  // Button hints. Overlay mode puts Cancel/Confirm on the first two slots (the Back and Confirm
  // buttons) and leaves Left/Right inert; full-screen mode keeps the original Left/Right layout.
  const auto labels = overlay
                          ? mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM), "", "")
                          : mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (overlay) {
    // Overlay layout (btn1/btn2): confirm with the Confirm button, cancel with Back. The Left/Right
    // buttons are intentionally inert here. Swallow a Confirm release that carried over from the
    // Confirm-driven menu that launched this dialog so it can't auto-confirm.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (lockNextConfirmRelease) {
        lockNextConfirmRelease = false;
        return;
      }
      ActivityResult res;
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult res;
      res.isCancelled = true;
      setResult(std::move(res));
      finish();
      return;
    }
    return;
  }

  // Full-screen layout (btn3/btn4): confirm with Right, cancel with Left.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}