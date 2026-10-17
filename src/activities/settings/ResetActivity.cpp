#include "ResetActivity.h"

#include <Arduino.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "util/Reset.h"

void ResetActivity::onEnter() {
  Activity::onEnter();
  showConfirmation(false);
  requestUpdate(true);
}

void ResetActivity::showConfirmation(const bool confirmAgain) {
  const char* title = nullptr;
  const char* warning = nullptr;
  switch (mode) {
    case Mode::SettingsOnly:
      title = tr(STR_RESET_SETTINGS);
      warning = tr(STR_RESET_SETTINGS_WARNING);
      break;
    case Mode::Factory:
      title = tr(STR_FACTORY_RESET);
      warning = tr(STR_FACTORY_RESET_WARNING);
      break;
  }
  if (confirmAgain) warning = tr(STR_RESET_CONFIRM_AGAIN);
  const char* options[] = {tr(STR_NO), tr(STR_YES)};
  confirmPopup.showMessage(title, warning, options, 2, 0, [this, confirmAgain](int idx) {
    if (idx != 1) {
      finish();
      return;
    }

    if (mappedInput.hasTouch() && !confirmAgain) {
      showConfirmation(true);
      return;
    }

    resetAndRestart();
  });
}

void ResetActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (confirmPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}

void ResetActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] {
        if (confirmPopup.isActive()) requestUpdate();
      }))
    return;
  finish();
}

void ResetActivity::resetAndRestart() {
  // Keep rendering and parent callbacks from reopening or rewriting erased data.
  RenderLock lock(*this);
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_FACTORY_RESETTING));
  bool success = false;
  switch (mode) {
    case Mode::SettingsOnly:
      success = resetCrossPointSettings();
      break;
    case Mode::Factory:
      success = resetCrossPointData();
      break;
  }
  renderer.clearScreen();
  GUI.drawPopup(renderer, success ? tr(STR_FACTORY_RESET_COMPLETE) : tr(STR_FACTORY_RESET_FAILED));
  delay(success ? 1500 : 5000);
  ESP.restart();
}
