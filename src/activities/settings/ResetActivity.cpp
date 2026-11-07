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
  const char* options[] = {tr(STR_NO), tr(STR_YES)};
  confirmPopup.showMessage(tr(STR_FACTORY_RESET), tr(STR_FACTORY_RESET_WARNING), options, 2, 0,
                           [this, confirmAgain](int idx) {
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
  const bool success = resetCrossPointData();
  renderer.clearScreen();
  GUI.drawPopup(renderer, success ? tr(STR_FACTORY_RESET_COMPLETE) : tr(STR_FACTORY_RESET_FAILED));
  delay(success ? 1500 : 5000);
  ESP.restart();
}
