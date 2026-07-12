#include "SwapBootSlotActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SwapBootSlotActivity::onEnter() {
  Activity::onEnter();

  if (!boot_switch::peekPassiveSlot(info)) {
    state = State::NO_TARGET;
    requestUpdate(true);
    return;
  }

  // Generic app descriptors make the two firmwares indistinguishable by name
  // (see BootSwitch.h), so the prompt names the slot plus best-effort version.
  char body[64];
  if (info.version[0] != '\0') {
    snprintf(body, sizeof(body), "%s: %s", info.label, info.version);
  } else {
    snprintf(body, sizeof(body), "%s", info.label);
  }

  state = State::CONFIRMING;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SWITCH_OS_PROMPT), std::string(body)),
      [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SwapBootSlotActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    dismiss();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::SWAPPING;
  }
  requestUpdateAndWait();

  if (!boot_switch::swapToPassive()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("BOOT", "otadata repointed at %s, restarting", info.label);
  // Hold the "Switching OS..." screen briefly so the user sees it took effect.
  delay(1000);
  ESP.restart();
}

void SwapBootSlotActivity::dismiss() {
  if (bootMode) {
    // Boot-hold entry installs this as the stack root; there is nothing to pop
    // back to, so leave via the home screen instead.
    onGoHome();
    return;
  }
  finish();
}

void SwapBootSlotActivity::loop() {
  switch (state) {
    case State::NO_TARGET:
    case State::FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        dismiss();
      }
      break;
    case State::CONFIRMING:  // sub-activity owns input
    case State::SWAPPING:    // restart imminent
      break;
  }
}

void SwapBootSlotActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SWITCH_OS));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  switch (state) {
    case State::NO_TARGET: {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SWITCH_OS_NO_TARGET), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::SWAPPING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SWITCH_OS_SWITCHING), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
      break;
    case State::FAILED: {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SWITCH_OS_FAILED), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::CONFIRMING:
      // ConfirmationActivity is on top; nothing to draw.
      break;
  }

  renderer.displayBuffer();
}
