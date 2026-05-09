#include "ConfirmationActivity.h"

#include <I18n.h>

#include "../../components/UITheme.h"
#include "HalDisplay.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body, const bool wrapBodyIn,
                                           const int wrapMaxLinesIn)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      wrapBody(wrapBodyIn),
      wrapMaxLines(wrapMaxLinesIn > 0 ? wrapMaxLinesIn : 10) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  safeBody.clear();
  bodyWrappedLines.clear();
  if (!body.empty()) {
    if (wrapBody) {
      bodyWrappedLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, wrapMaxLines, EpdFontFamily::REGULAR);
    } else {
      safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
    }
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) {
    totalHeight += lineHeight;
  }
  const bool hasBody = !safeBody.empty() || !bodyWrappedLines.empty();
  if (!safeHeading.empty() && hasBody) {
    totalHeight += spacing;
  }
  if (!bodyWrappedLines.empty()) {
    totalHeight += static_cast<int>(bodyWrappedLines.size()) * lineHeight;
  } else if (!safeBody.empty()) {
    totalHeight += lineHeight;
  }

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

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

  // Draw body (single truncated line, or word-wrapped lines)
  if (!bodyWrappedLines.empty()) {
    for (const auto& line : bodyWrappedLines) {
      renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
      currentY += lineHeight;
    }
  } else if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Draw UI Elements
  const auto labels = mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
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