#include "FrontlightPanelActivity.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr int BUTTON_BRIGHTNESS_STEP = 5;
constexpr int FINE_STEP = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  dimStep = SETTINGS.frontlightNightLight != 0 && Frontlight.supportsNightLight() ? Frontlight.dimStep() : 0;
  // Night Light was toggled off while a dim step was live: return the
  // hardware to percent mode so the panel and the light agree again.
  // cppcheck-suppress knownConditionTrueFalse
  if (dimStep == 0 && Frontlight.dimStep() > 0) {
    Frontlight.setBrightness(brightness);
  }
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       SETTINGS.frontlightDimStep != dimStep ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    SETTINGS.frontlightDimStep = dimStep;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = percentFromPermille(event.dragPermille);
  self->dimStep = 0;  // an absolute slider gesture always returns to percent mode
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value * FINE_STEP);
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  // Night Light: below the 1% floor the control walks a fixed sub-1% ladder
  // one step per press (regardless of the button's percent step size), and
  // the dimmest step is the floor — off stays on the sun toggle.
  const bool nightLight = SETTINGS.frontlightNightLight != 0 && Frontlight.supportsNightLight();
  // cppcheck-suppress knownConditionTrueFalse
  if (nightLight && (dimStep > 0 || (delta < 0 && brightness <= 1))) {
    int nextStep = dimStep;
    if (delta < 0) {
      nextStep = dimStep < HalFrontlight::DIM_STEP_COUNT ? dimStep + 1 : dimStep;
    } else if (delta > 0) {
      nextStep = dimStep - 1;  // step 1 exits the ladder back to 1%
    }
    if (nextStep == dimStep) return;
    dimStep = static_cast<uint8_t>(nextStep);
    if (dimStep == 0) {
      brightness = 1;
      Frontlight.setBrightness(brightness);
    } else {
      Frontlight.setDimStep(dimStep);
    }
    if (!lightOn) {
      lightOn = true;
      lightOnChanged = true;
      Frontlight.setOn(true);
    }
    requestUpdate();
    return;
  }

  int next = static_cast<int>(brightness) + delta;
  // With Night Light on, stepping down lands on 1% first so the ladder is
  // always entered from the same place; without it, 0 remains reachable.
  // cppcheck-suppress knownConditionTrueFalse
  const int floor = nightLight && delta < 0 ? 1 : 0;
  if (next < floor) next = floor;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
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
      if (snap.touchReleased && !draggingSlider && snap.touchY >= panelBottom) {
        close();
        return;
      }
    }
    if (draggingSlider) {
      if (!snap.touchHeld) draggingSlider = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BUTTON_BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BUTTON_BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lineHeight = uiTarget.lineHeight(tokens.bodyText.font);
  int y = metrics.topPadding + metrics.headerHeight;
  y += tokens.spaceLg;
  y += tokens.rowHeight + tokens.spaceSm;
  y += tokens.rowHeight + tokens.spaceLg;
  if (Frontlight.hasColorTemperature()) {
    y += lineHeight + tokens.spaceSm + tokens.rowHeight + tokens.spaceLg;
  }
  y += tokens.spaceLg;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::buildPanelScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - panelBottom);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0, bottomInset, 0});

  const int16_t lineHeight = screen.target().lineHeight(theme.bodyText.font);
  const int16_t rowHeight = theme.rowHeight;
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  char line[48];

  screen.spacer(theme.spaceLg);

  const fui::Rect headerRow = screen.takeTop(rowHeight, theme.spaceSm).inset(sideInset);
  if (dimStep > 0) {
    // Ladder percentages at 10-bit PWM (every in-tree LEDC frontlight profile);
    // labels track DIM_LEVELS.
    static const char* const kDimLabels[HalFrontlight::DIM_STEP_COUNT] = {"0.8", "0.6", "0.4", "0.2", "0.1"};
    snprintf(line, sizeof(line), "%s  %s%%", tr(STR_BRIGHTNESS), kDimLabels[dimStep - 1]);
  } else {
    snprintf(line, sizeof(line), "%s  %u%%", tr(STR_BRIGHTNESS), static_cast<unsigned>(brightness));
  }
  const fui::BitmapRef sunIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  const int16_t iconWidth = static_cast<int16_t>(sunIcon.width);
  const int16_t iconHeight = static_cast<int16_t>(sunIcon.height);
  const int16_t controlWidth = static_cast<int16_t>(iconWidth + theme.spaceLg * 2);
  const fui::Rect sunHit{static_cast<int16_t>(headerRow.right() - controlWidth), headerRow.y, controlWidth, rowHeight};
  const fui::Rect sunRect{static_cast<int16_t>(sunHit.x + (controlWidth - iconWidth) / 2),
                          static_cast<int16_t>(headerRow.y + (rowHeight - iconHeight) / 2), iconWidth, iconHeight};
  const fui::Rect labelRect{headerRow.x, static_cast<int16_t>(headerRow.y + (rowHeight - lineHeight) / 2),
                            static_cast<int16_t>(headerRow.width - controlWidth - theme.spaceMd), lineHeight};
  screen.target().text(labelRect, line, theme.bodyText);
  screen.frame().hit(sunHit, ACTION_TOGGLE);
  screen.target().bitmap(sunRect, sunIcon, fui::BitmapMode::Center);

  addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), dimStep > 0 ? 0 : brightness,
                ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP);

  if (Frontlight.hasColorTemperature()) {
    snprintf(line, sizeof(line), "%s  %u%%", tr(STR_WARMTH), static_cast<unsigned>(warmth));
    screen.target().text(screen.takeTop(lineHeight, theme.spaceSm).inset(sideInset), line, theme.bodyText);
    addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), warmth, ACTION_WARMTH,
                  ACTION_WARMTH_STEP);
  }

  screen.spacer(theme.spaceLg);
}

void FrontlightPanelActivity::addStepSlider(UiApp::ScreenType& screen, const fui::Rect& row, const uint8_t value,
                                            const fui::ActionId sliderAction, const fui::ActionId stepAction) {
  const auto& theme = screen.theme();
  const int16_t stepWidth = row.height;
  const fui::Rect minusHit{row.x, row.y, stepWidth, row.height};
  const fui::Rect plusHit{static_cast<int16_t>(row.right() - stepWidth), row.y, stepWidth, row.height};

  fui::TextStyle glyph = theme.bodyText;
  glyph.align = fui::TextAlign::Center;
  const int16_t lineHeight = screen.target().lineHeight(glyph.font);
  const int16_t glyphY = static_cast<int16_t>(row.y + (row.height - lineHeight) / 2);
  screen.target().text(fui::Rect{minusHit.x, glyphY, stepWidth, lineHeight}, "-", glyph);
  screen.target().text(fui::Rect{plusHit.x, glyphY, stepWidth, lineHeight}, "+", glyph);
  screen.frame().hit(minusHit, stepAction, -1, fui::InputTouch);
  screen.frame().hit(plusHit, stepAction, 1, fui::InputTouch);

  fui::SliderProps props;
  props.value = value;
  props.max = 100;
  props.action = sliderAction;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  const int16_t sideGap = static_cast<int16_t>(stepWidth + theme.spaceSm);
  fui::slider(screen.frame(), row.inset(fui::Insets{0, sideGap, 0, sideGap}), props);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();
  const int pageWidth = renderer.getScreenWidth();
  renderer.fillRect(0, 0, pageWidth, panelBottom, false);

  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRONTLIGHT));

  uiReady = false;
  app.render();
  uiReady = true;

  renderer.fillRect(0, panelBottom - 2, pageWidth, 2, true);
  renderer.displayBuffer();
}
