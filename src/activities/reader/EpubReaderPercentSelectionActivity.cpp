#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
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
  // Wrap using a 100-value ring (0% and 100% are the same wrap point), but keep 100 as the
  // natural landing value when reached without crossing the boundary (e.g. 90 + 10 = 100).
  const int raw = percent + delta;
  if (raw > 0 && raw % 100 == 0) {
    percent = 100;
  } else {
    percent = ((raw % 100) + 100) % 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::loop() {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = screen.x + (screen.width - barWidth) / 2;
  const int barY = contentTop + metrics.verticalSpacing * 2;
  int tx = 0;
  int ty = 0;

  // Live drag on the slider: once a touch lands on the bar, the percent follows the
  // finger until release. Runs before the Back handler because the release of a drag
  // can also register as a swipe (e.g. the left-edge rightward back gesture) — the
  // drag must consume it so it can't cancel the dialog or step the percent.
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    if (draggingBar ||
        (tx >= barX - 20 && tx < barX + barWidth + 20 && ty >= barY - 24 && ty < barY + barHeight + 24)) {
      draggingBar = true;
      const int dragged = std::clamp((tx - barX) * 100 / barWidth, 0, 100);
      if (dragged != percent) {
        percent = dragged;
        requestUpdate();
      }
      return;
    }
  } else if (draggingBar) {
    // Release frame of a drag: swallow the tap/swipe events it produced.
    draggingBar = false;
    return;
  }

  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasScreenTapped(tx, ty) && tx >= barX - 20 && tx < barX + barWidth + 20 && ty >= barY - 24 &&
      ty < barY + barHeight + 24) {
    percent = std::clamp((tx - barX) * 100 / barWidth, 0, 100);
    requestUpdate();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Right) {
    adjustPercent(kLargeStep);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left) {
    adjustPercent(-kLargeStep);
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
