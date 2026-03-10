#include "PromptActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"

PromptActivity::PromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string message,
                               std::function<void()> onConfirm, std::function<void()> onCancel)
    : Activity("Prompt", renderer, mappedInput),
      message(std::move(message)),
      onConfirm(std::move(onConfirm)),
      onCancel(std::move(onCancel)) {}

void PromptActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PromptActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onConfirm) onConfirm();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onCancel) onCancel();
  }
}

void PromptActivity::render(RenderLock&&) {
  GUI.drawPopup(renderer, message.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}