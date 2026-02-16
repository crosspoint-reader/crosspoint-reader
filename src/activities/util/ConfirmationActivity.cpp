#include "ConfirmationActivity.h"
#include "HalDisplay.h"          
#include <I18n.h>
#include "../../fontIds.h"         
#include "../../components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::string& message,
                                          std::function<void(bool)> onResult)
    : Activity("Confirmation", renderer, mappedInput),
      message(message), onResult(onResult) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  renderer.clearScreen();
  
  // Handle Text Overflow
  const int margin = 20;
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  std::string safeMessage = renderer.truncatedText(UI_10_FONT_ID, message.c_str(), maxWidth);

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (renderer.getScreenHeight() - height) / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, top, safeMessage.c_str(), true);

  const auto labels = mappedInput.mapLabels(
      I18N.get(StrId::STR_CANCEL), 
      I18N.get(StrId::STR_CONFIRM), 
      "", ""
  );

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  // Snappy, single-click, and safe because it waits for the release.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onResult) onResult(true);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onResult) onResult(false);
  }
}