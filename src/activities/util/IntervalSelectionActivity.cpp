#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_SLIDER = 1;
constexpr fui::ActionId ACTION_STEP = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr fui::ActionId ACTION_OK = 4;
}  // namespace

IntervalSelectionActivity::IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const char* activityName, const StrId titleId,
                                                     const int initialValue, const int minValue, const int maxValue,
                                                     const int smallStep, const int largeStep,
                                                     const StrId valueFormatId, const bool readerActivity,
                                                     const bool ignoreInitialConfirmRelease,
                                                     const StrId maxBoundaryLabelId)
    : Activity(activityName, renderer, mappedInput),
      titleId(titleId),
      valueFormatId(valueFormatId),
      maxBoundaryLabelId(maxBoundaryLabelId),
      value(initialValue),
      minValue(minValue),
      maxValue(maxValue),
      smallStep(smallStep),
      largeStep(largeStep),
      readerActivity(readerActivity),
      ignoreConfirmRelease(ignoreInitialConfirmRelease),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_SLIDER, &IntervalSelectionActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &IntervalSelectionActivity::onStepEvent, this);
  app.on(ACTION_CANCEL, &IntervalSelectionActivity::onCancelEvent, this);
  app.on(ACTION_OK, &IntervalSelectionActivity::onOkEvent, this);
  app.setScreen(&IntervalSelectionActivity::intervalScreen, this);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::setValue(const int candidate) {
  const int clamped = clampedValue(candidate);
  if (clamped == value) return;
  value = clamped;
  requestUpdate();
}

void IntervalSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void IntervalSelectionActivity::confirm() {
  setResult(IntervalResult{static_cast<uint32_t>(value)});
  finish();
}

void IntervalSelectionActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  if (event.dragPermille < 0) return;
  const int range = std::max(1, self->maxValue - self->minValue);
  self->setValue(self->minValue + (static_cast<int>(event.dragPermille) * range + 500) / 1000);
}

void IntervalSelectionActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->adjustValue(event.value * self->smallStep);
}

void IntervalSelectionActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->app.clearTapFlash();  // the tap leaves this screen
  self->cancel();
}

void IntervalSelectionActivity::onOkEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<IntervalSelectionActivity*>(user);
  self->app.clearTapFlash();  // the tap leaves this screen
  self->confirm();
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

  // Touch goes through the FreeInkApp: render() registered the slider, -/+ zones,
  // and Cancel/OK hit rects; the slider follows the finger via InputDrag. Runs
  // before the Back handler because the release of a drag can also register as a
  // swipe (e.g. the left-edge rightward back gesture) — the drag must consume it
  // so it can't cancel the dialog.
  fui::InputSnapshot snap{};
  if (uiReady) {
    snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) {
        if (event.dragPermille >= 0) draggingSlider = true;
        return;
      }
    }
    if (draggingSlider) {
      // Drag ended (possibly off the slider): swallow the tap/swipe events it produced.
      if (!snap.touchHeld) draggingSlider = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });

  // On edge-button boards (X3, X4 Pro) the side buttons sit on the left/right edges of the screen rather
  // than as a vertical up/down rocker (X4), so BTN_UP is physically the left button and BTN_DOWN the right
  // one. Flip the large-step direction there so the left button decreases and the right button increases.
  const int upDelta = gpio.hasEdgeSideButtons() ? -largeStep : largeStep;
  const int downDelta = gpio.hasEdgeSideButtons() ? largeStep : -largeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustValue(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustValue(downDelta); });
}

void IntervalSelectionActivity::formatValue(char* buffer, const size_t size, const int forValue) const {
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && forValue == maxValue) {
    snprintf(buffer, size, "%s", I18N.get(maxBoundaryLabelId));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(buffer, size, I18N.get(valueFormatId), static_cast<unsigned int>(forValue));
  } else {
    snprintf(buffer, size, "%d", forValue);
  }
}

void IntervalSelectionActivity::intervalScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<IntervalSelectionActivity*>(user)->buildIntervalScreen(screen);
}

void IntervalSelectionActivity::buildIntervalScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints, mirroring the
  // percent-selection dialog's layout.
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  char line[64];

  // Value readout, centered above the slider.
  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutLh = screen.target().lineHeight(readout.font);
  formatValue(line, sizeof(line), value);
  screen.target().text(screen.takeTop(readoutLh, theme.spaceLg), line, readout);

  // Slider row: -/+ tap zones at the row ends for fine steps (a full-row-height
  // square each, comfortable touch targets), with the drag slider between them.
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  const fui::Rect row = screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset);
  const int16_t stepW = row.height;
  const fui::Rect minusHit{row.x, row.y, stepW, row.height};
  const fui::Rect plusHit{static_cast<int16_t>(row.right() - stepW), row.y, stepW, row.height};

  fui::TextStyle glyph = theme.bodyText;
  glyph.align = fui::TextAlign::Center;
  const int16_t glyphLh = screen.target().lineHeight(glyph.font);
  const int16_t glyphY = static_cast<int16_t>(row.y + (row.height - glyphLh) / 2);
  screen.target().text(fui::Rect{minusHit.x, glyphY, stepW, glyphLh}, "-", glyph);
  screen.target().text(fui::Rect{plusHit.x, glyphY, stepW, glyphLh}, "+", glyph);
  screen.frame().hit(minusHit, ACTION_STEP, -1, fui::InputTouch);
  screen.frame().hit(plusHit, ACTION_STEP, +1, fui::InputTouch);

  const int range = std::max(1, maxValue - minValue);
  fui::SliderProps props;
  props.value = value - minValue;
  props.max = range;
  props.action = ACTION_SLIDER;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  const int16_t sideGap = static_cast<int16_t>(stepW + theme.spaceSm);
  fui::slider(screen.frame(), row.inset(fui::Insets{0, sideGap, 0, sideGap}), props);

  if (mappedInput.hasTouch()) {
    // Touch devices drive the slider directly and confirm/cancel on screen; the
    // physical-button step hints are hidden there — same rule as GUI.drawButtonHints.
    addDialogCancelOk(screen, ACTION_CANCEL, ACTION_OK);
    return;
  }

  // Two-line step hint: front buttons do the small step, side buttons the large step. Built from
  // separate label + value strings (rather than splitting one localized sentence) so the layout
  // doesn't depend on translators preserving a hidden separator.
  fui::TextStyle hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  const int16_t hintLh = screen.target().lineHeight(hint.font);
  char stepText[24];
  for (const auto& [labelId, step] :
       {std::pair{StrId::STR_STEP_HINT_FRONT, smallStep}, std::pair{StrId::STR_STEP_HINT_SIDE, largeStep}}) {
    if (valueFormatId != StrId::STR_NONE_OPT) {
      snprintf(stepText, sizeof(stepText), I18N.get(valueFormatId), static_cast<unsigned int>(step));
    } else {
      snprintf(stepText, sizeof(stepText), "%d", step);
    }
    snprintf(line, sizeof(line), "%s %s", I18N.get(labelId), stepText);
    screen.target().text(screen.takeTop(hintLh, theme.spaceSm), line, hint);
  }
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, I18N.get(titleId), true, EpdFontFamily::BOLD);

  // Value readout, slider, hints, and the touch Cancel/OK pair render through the
  // app so the interactive elements register touch hit rects.
  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
