#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::drawStepHintLine(const int y, const StrId labelId, const int step) {
  char stepText[24];
  if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(stepText, sizeof(stepText), I18N.get(valueFormatId), static_cast<unsigned int>(step));
  } else {
    snprintf(stepText, sizeof(stepText), "%d", step);
  }
  char line[64];
  snprintf(line, sizeof(line), "%s %s", I18N.get(labelId), stepText);
  renderer.drawCenteredText(SMALL_FONT_ID, y, line, true);
}

void IntervalSelectionActivity::loop() {
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });

  // On X3 the side buttons sit on the left/right edges of the screen rather than as a vertical up/down
  // rocker (X4), so BTN_UP is physically the left button and BTN_DOWN the right one. Flip the large-step
  // direction there so the left button decreases and the right button increases, matching the layout.
  const int upDelta = gpio.deviceIsX3() ? -largeStep : largeStep;
  const int downDelta = gpio.deviceIsX3() ? largeStep : -largeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustValue(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustValue(downDelta); });
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(titleId), true, EpdFontFamily::BOLD);

  char formattedValue[32];
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && value == maxValue) {
    snprintf(formattedValue, sizeof(formattedValue), "%s", I18N.get(maxBoundaryLabelId));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(formattedValue, sizeof(formattedValue), I18N.get(valueFormatId), static_cast<unsigned int>(value));
  } else {
    snprintf(formattedValue, sizeof(formattedValue), "%d", value);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, 90, formattedValue, true, EpdFontFamily::BOLD);

  const int screenWidth = renderer.getScreenWidth();
  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 16;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = (barWidth - 4) * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = std::max(barX + 2, barX + 2 + fillWidth - 2);
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Two-line step hint: front buttons do the small step, side buttons the large step. Built from
  // separate label + value strings (rather than splitting one localized sentence) so the layout
  // doesn't depend on translators preserving a hidden separator.
  drawStepHintLine(barY + 30, StrId::STR_STEP_HINT_FRONT, smallStep);
  drawStepHintLine(barY + 52, StrId::STR_STEP_HINT_SIDE, largeStep);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
