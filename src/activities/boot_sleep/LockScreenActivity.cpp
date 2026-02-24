#include "LockScreenActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

extern void enterDeepSleep();

namespace {
constexpr unsigned long kErrorDisplayMs = 1200;
constexpr int kDotRadius = 8;
constexpr int kDotSpacing = 28;
}  // namespace

void LockScreenActivity::onEnter() {
  Activity::onEnter();
  inputLength = 0;
  errorUntil = 0;
  unlockPending = false;
  requestUpdate();
}

void LockScreenActivity::loop() {
  // Wait for all buttons to be released before unlocking.
  // This prevents the release event from leaking into the next activity.
  if (unlockPending) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.isPressed(MappedInputManager::Button::Left) &&
        !mappedInput.isPressed(MappedInputManager::Button::Right) &&
        !mappedInput.isPressed(MappedInputManager::Button::Up) &&
        !mappedInput.isPressed(MappedInputManager::Button::Down)) {
      onUnlocked();
    }
    return;
  }

  // Clear error state after timeout
  if (errorUntil > 0 && millis() > errorUntil) {
    inputLength = 0;
    errorUntil = 0;
    requestUpdate();
    return;
  }

  // Don't accept input while showing error
  if (errorUntil > 0) return;

  // Power long-press → go back to sleep
  if (mappedInput.isPressed(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    return;
  }

  // Detect button presses using physical positions (bypasses remapping)
  int pressed = -1;
  int frontBtn = mappedInput.getPressedFrontButton();
  if (frontBtn >= 0 && frontBtn <= 3) {
    pressed = frontBtn;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    pressed = 4;
  }

  if (pressed < 0) return;

  inputBuffer[inputLength] = static_cast<uint8_t>(pressed);
  inputLength++;
  requestUpdate();

  if (inputLength == SETTINGS.lockSequenceLength) {
    if (memcmp(inputBuffer, SETTINGS.lockSequence, SETTINGS.lockSequenceLength) == 0) {
      unlockPending = true;
    } else {
      errorUntil = millis() + kErrorDisplayMs;
      requestUpdate();
    }
  }
}

void LockScreenActivity::render(Activity::RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Sleep screen style background
  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2 - 60, 120, 120);

  // Draw "LOCKED" text centered
  const int textY = pageHeight / 2 + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_LOCKED), true, EpdFontFamily::BOLD);

  // Draw progress dots
  const int seqLen = SETTINGS.lockSequenceLength;
  const int totalWidth = seqLen * kDotRadius * 2 + (seqLen - 1) * (kDotSpacing - kDotRadius * 2);
  const int startX = (pageWidth - totalWidth) / 2;
  const int dotY = pageHeight / 2 + 60;

  for (int i = 0; i < seqLen; i++) {
    const int cx = startX + i * kDotSpacing + kDotRadius;
    const int cy = dotY;

    if (i < inputLength) {
      renderer.fillRoundedRect(cx - kDotRadius, cy - kDotRadius, kDotRadius * 2, kDotRadius * 2, kDotRadius,
                               Color::Black);
    } else {
      renderer.drawRoundedRect(cx - kDotRadius, cy - kDotRadius, kDotRadius * 2, kDotRadius * 2, 1, kDotRadius, true);
    }
  }

  // Show error message if wrong code was entered
  if (errorUntil > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, dotY + 30, tr(STR_WRONG_CODE), true);
  }

  const char* frontLabels[] = {ButtonShape::CIRCLE, ButtonShape::SQUARE, ButtonShape::TRIANGLE, ButtonShape::CROSS};
  if (SETTINGS.frontButtonBack < 4) frontLabels[SETTINGS.frontButtonBack] = "";
  GUI.drawButtonHints(renderer, frontLabels[0], frontLabels[1], frontLabels[2], frontLabels[3]);
  GUI.drawSideButtonHints(renderer, ButtonShape::DIAMOND, "");

  renderer.displayBuffer();
}
