#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PercentResult{percent});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  // On X3 the side buttons sit on the left/right edges of the screen rather than as a vertical up/down
  // rocker (X4), so BTN_UP is physically the left button and BTN_DOWN the right one. Flip the large-step
  // direction there so the left button decreases and the right button increases, matching the layout.
  const int upDelta = gpio.deviceIsX3() ? -kLargeStep : kLargeStep;
  const int downDelta = gpio.deviceIsX3() ? kLargeStep : -kLargeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustPercent(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustPercent(downDelta); });
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_GO_TO_PERCENT));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;

  const std::string percentText = std::to_string(percent) + "%";
  UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, contentTop, percentText.c_str(), true,
                            EpdFontFamily::BOLD);

  // Draw slider track.
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = screen.x + (screen.width - barWidth) / 2;
  const int barY = contentTop + metrics.verticalSpacing * 2;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on percent.
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current percent.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Two-line step hint built from separate label + value strings (front buttons = fine step, side
  // buttons = coarse step), so the layout doesn't depend on a separator hidden in translated text.
  char line[64];
  snprintf(line, sizeof(line), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_FRONT), kSmallStep);
  UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, barY + 30, line, true);
  snprintf(line, sizeof(line), "%s %d%%", I18N.get(StrId::STR_STEP_HINT_SIDE), kLargeStep);
  UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, barY + 52, line, true);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
