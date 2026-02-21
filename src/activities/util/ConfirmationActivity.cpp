#include "ConfirmationActivity.h"

#include <I18n.h>

#include "../../components/UITheme.h"
#include "../../fontIds.h"
#include "HalDisplay.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           std::function<void(bool)> onResult)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body), onResult(onResult) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  renderer.clearScreen();

  const int margin = 20;
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  const int fontId = UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int spacing = 30;
  bool hasHeading = !heading.empty();
  bool hasBody = !body.empty();

  // Calculate total height based on what exists
  int totalHeight = 0;
  if (hasHeading) totalHeight += lineHeight;
  if (hasBody) totalHeight += lineHeight;
  if (hasHeading && hasBody) totalHeight += spacing;
  int currentY = (renderer.getScreenHeight() - totalHeight) / 2;

  // Render Heading if it exists
  if (hasHeading) {
    std::string safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Render Body if it exists
  if (hasBody) {
    std::string safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onResult) onResult(true);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onResult) onResult(false);
  }
}