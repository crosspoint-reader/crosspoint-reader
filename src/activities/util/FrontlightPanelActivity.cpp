#include "FrontlightPanelActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
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
constexpr fui::ActionId ACTION_INVERT = 4;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 5;
constexpr fui::ActionId ACTION_WARMTH_STEP = 6;
constexpr int BRIGHTNESS_STEP = 5;
// Fine adjustment per -/+ tap on the slider end zones.
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

  // The HAL is the live source of truth; SETTINGS only mirrors it for boot.
  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  // Night mode is a reader preference now (ActivityManager applies it per
  // render, reading surfaces only), so the setting is the source of truth —
  // the display flag just reflects whatever screen rendered last.
  inverted = SETTINGS.screenInverted != 0;

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_INVERT, &FrontlightPanelActivity::onInvertEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // Debounced persistence: one SPIFFS write on close, never per slider tick.
  if (SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
      SETTINGS.frontlightOn != (lightOn ? 1 : 0)) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = percentFromPermille(event.dragPermille);
  Frontlight.setBrightness(self->brightness);
  // Adjusting brightness while off is an obvious "I want light" intent.
  if (!self->lightOn) {
    self->lightOn = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onInvertEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleInversion();
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  // Mirrors the warmth slider handler: no light-on forcing — warmth alone is
  // not an "I want light" intent the way brightness is.
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
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::toggleInversion() {
  inverted = SETTINGS.screenInverted == 0;
  SETTINGS.screenInverted = inverted ? 1 : 0;
  SETTINGS.saveToFile();
  // No direct display flip: the reader repaints inverted when this overlay
  // closes (ActivityManager resolves polarity per render).
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  // The X4 Pro power-button double-click can change the light globally while
  // this panel is open. Keep the panel's state and eventual persistence in sync.
  if (lightOn != Frontlight.isOn()) {
    lightOn = Frontlight.isOn();
    requestUpdate();
  }

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
      // No control took it. A tap released at/below the panel edge (in the
      // preserved content underneath) dismisses — the drop-down behaves like a
      // modal scrim. A drag-off release carries y = -1, so it never dismisses.
      if (snap.touchReleased && !draggingSlider && snap.touchY >= panelBottom) {
        close();
        return;
      }
    }
    if (draggingSlider) {
      // Drag ended (possibly off the slider): swallow the release's swipe so
      // it can't double as the left-edge back gesture and close the panel.
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
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  // Mirror buildPanelScreen's takeTop/spacer sequence so the frame, content
  // margin, and dismiss threshold land on the same edge.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lh = uiTarget.lineHeight(tokens.bodyText.font);
  int y = metrics.topPadding + metrics.headerHeight;
  y += tokens.spaceLg;                     // leading spacer
  y += tokens.rowHeight + tokens.spaceSm;  // brightness label + sun toggle row
  y += tokens.rowHeight + tokens.spaceLg;  // brightness slider
  if (Frontlight.hasColorTemperature()) {
    y += lh + tokens.spaceSm + tokens.rowHeight + tokens.spaceLg;  // warmth label + slider
  }
  y += tokens.spaceLg;  // trailing padding
  return y;
}

void FrontlightPanelActivity::panelScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::buildPanelScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  // Body spans from below the header down to the drop-down's bottom edge, so
  // the app never lays out over the preserved content underneath.
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - panelBottom);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0, bottomInset, 0});

  const int16_t lh = screen.target().lineHeight(theme.bodyText.font);
  const int16_t rowH = theme.rowHeight;
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  char line[48];

  screen.spacer(theme.spaceLg);

  // Header row: "Brightness NN%" on the left, with direct inversion and
  // frontlight controls on the right. Each 32px glyph sits in a generous,
  // non-overlapping hit band spanning the full row height.
  const fui::Rect headerRow = screen.takeTop(rowH, theme.spaceSm).inset(sideInset);
  snprintf(line, sizeof(line), "%s  %u%%", tr(STR_BRIGHTNESS), static_cast<unsigned>(brightness));
  // Filled glyphs signal the active state: solid sun = light on, solid moon = inverted.
  const fui::BitmapRef sunIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  const fui::BitmapRef moonIcon = fui::bitmapFromIcon(inverted ? icon_moon_filled_32 : icon_moon_32);
  const int16_t iconW = static_cast<int16_t>(sunIcon.width);
  const int16_t iconH = static_cast<int16_t>(sunIcon.height);
  const int16_t controlW = static_cast<int16_t>(iconW + theme.spaceLg * 2);
  const fui::Rect sunHit{static_cast<int16_t>(headerRow.right() - controlW), headerRow.y, controlW, rowH};
  const fui::Rect invertHit{static_cast<int16_t>(sunHit.x - controlW), headerRow.y, controlW, rowH};
  const fui::Rect sunRect{static_cast<int16_t>(sunHit.x + (controlW - iconW) / 2),
                          static_cast<int16_t>(headerRow.y + (rowH - iconH) / 2), iconW, iconH};
  const fui::Rect moonRect{static_cast<int16_t>(invertHit.x + (controlW - iconW) / 2),
                           static_cast<int16_t>(headerRow.y + (rowH - iconH) / 2), iconW, iconH};
  const fui::Rect labelRect{headerRow.x, static_cast<int16_t>(headerRow.y + (rowH - lh) / 2),
                            static_cast<int16_t>(headerRow.width - controlW * 2 - theme.spaceMd), lh};
  screen.target().text(labelRect, line, theme.bodyText);
  screen.frame().hit(invertHit, ACTION_INVERT, 0, fui::InputTouch, inverted ? fui::StateSelected : fui::StateNormal);
  screen.frame().hit(sunHit, ACTION_TOGGLE);
  screen.target().bitmap(moonRect, moonIcon, fui::BitmapMode::Center);
  screen.target().bitmap(sunRect, sunIcon, fui::BitmapMode::Center);

  addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), brightness, ACTION_BRIGHTNESS,
                ACTION_BRIGHTNESS_STEP);

  if (Frontlight.hasColorTemperature()) {
    snprintf(line, sizeof(line), "%s  %u%%", tr(STR_WARMTH), static_cast<unsigned>(warmth));
    screen.target().text(screen.takeTop(lh, theme.spaceSm).inset(sideInset), line, theme.bodyText);
    addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), warmth, ACTION_WARMTH,
                  ACTION_WARMTH_STEP);
  }

  screen.spacer(theme.spaceLg);
}

void FrontlightPanelActivity::addStepSlider(UiApp::ScreenType& screen, const fui::Rect& row, const uint8_t value,
                                            const fui::ActionId sliderAction, const fui::ActionId stepAction) {
  const auto& theme = screen.theme();

  // -/+ tap zones at the row ends for 1% fine steps: a full-row-height square
  // each, so they stay comfortable touch targets. The slider sits between them
  // with a small gap, so its (min-touch-expanded) hit rect cannot overlap.
  const int16_t stepW = row.height;
  const fui::Rect minusHit{row.x, row.y, stepW, row.height};
  const fui::Rect plusHit{static_cast<int16_t>(row.right() - stepW), row.y, stepW, row.height};

  fui::TextStyle glyph = theme.bodyText;
  glyph.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(glyph.font);
  const int16_t glyphY = static_cast<int16_t>(row.y + (row.height - lh) / 2);
  screen.target().text(fui::Rect{minusHit.x, glyphY, stepW, lh}, "-", glyph);
  screen.target().text(fui::Rect{plusHit.x, glyphY, stepW, lh}, "+", glyph);
  screen.frame().hit(minusHit, stepAction, -1, fui::InputTouch);
  screen.frame().hit(plusHit, stepAction, +1, fui::InputTouch);

  fui::SliderProps props;
  props.value = value;
  props.max = 100;
  props.action = sliderAction;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  const int16_t sideGap = static_cast<int16_t>(stepW + theme.spaceSm);
  fui::slider(screen.frame(), row.inset(fui::Insets{0, sideGap, 0, sideGap}), props);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  // Overlay drop-down: keep the framebuffer content (the reader/menu we opened
  // over) intact below the panel; only the top third is repainted. No full
  // clearScreen — same as the theme popups draw over the current frame.
  panelBottom = computePanelBottom();
  const int pageWidth = renderer.getScreenWidth();
  renderer.fillRect(0, 0, pageWidth, panelBottom, false);  // white the panel area only

  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRONTLIGHT));

  uiReady = false;
  app.render();
  uiReady = true;

  // Bottom edge of the drop-down: a solid rule separating it from the content
  // (the scrim) underneath, reinforcing that a tap below dismisses.
  renderer.fillRect(0, panelBottom - 2, pageWidth, 2, true);

  renderer.displayBuffer();
}
