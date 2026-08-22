#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "util/ScreenshotUtil.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a 2-column tile grid.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberWidth = 72;
constexpr int16_t kGrabberHeight = 5;
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kSliderTrackRadius = 26;
constexpr int16_t kTileHeight = 84;
constexpr int16_t kTileRadius = 18;
constexpr int16_t kTileGap = 12;
constexpr int kTileCols = 2;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

// Which quick-setting tiles the panel shows is a user setting (Settings ->
// Display -> Customise Control Center). Order matches the tile ids runTile()
// switches on; the flags are read live so a change shows on the next open.
// Writes the ids of the enabled tiles into out (capacity kTileCount) and
// returns how many there are.
int visibleTiles(const MappedInputManager& input, int* out) {
  // The tiles are touch targets and every entry point to this panel is a touch
  // gesture; on a buttons-only board the panel stays what it originally was --
  // the frontlight sliders -- rather than presenting a grid nothing can tap.
  if (!input.hasTouch()) return 0;
  static uint8_t CrossPointSettings::* const flags[] = {
      &CrossPointSettings::ccTileNightMode,   &CrossPointSettings::ccTileRefresh,
      &CrossPointSettings::ccTileOrientation, &CrossPointSettings::ccTileTouch,
      &CrossPointSettings::ccTileScreenshot,  &CrossPointSettings::ccTileSleep,
  };
  int count = 0;
  for (int i = 0; i < static_cast<int>(std::size(flags)); ++i) {
    if (SETTINGS.*(flags[i])) out[count++] = i;
  }
  return count;
}

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}

// Both style sets below are compile-time constants living in flash rather than
// values a render builds: fui::StyleSet is 324 bytes, well past the 256-byte
// budget a local gets (AGENTS.md), and neither varies at runtime.
//
// Slider step buttons: same outlined-card language as the tiles, inverted while
// pressed so a tap is visibly acknowledged on a slow panel.
constexpr fui::StyleSet makeStepStyles() {
  fui::StyleSet s{};
  s.explicitlySet = true;
  s.normal.background = fui::Paint::solid(fui::Color::White);
  s.normal.foreground = fui::Paint::solid(fui::Color::Black);
  s.normal.border = fui::Paint::solid(fui::Color::Black);
  s.normal.borderWidth = 2;
  s.normal.radius = kTileRadius;
  s.selected = s.normal;
  s.focused = s.normal;
  s.active = s.normal;
  s.active.background = fui::Paint::solid(fui::Color::Black);
  s.active.foreground = fui::Paint::solid(fui::Color::White);
  s.disabled = s.normal;
  return s;
}

// 1-bit tile styling: an outlined card, filled solid black when the setting it
// carries is on (the "checked" state), never a dithered gray.
constexpr fui::StyleSet makeTileStyles() {
  fui::StyleSet s{};
  s.explicitlySet = true;
  s.normal.background = fui::Paint::solid(fui::Color::White);
  s.normal.foreground = fui::Paint::solid(fui::Color::Black);
  s.normal.border = fui::Paint::solid(fui::Color::Black);
  s.normal.borderWidth = 2;
  s.normal.radius = kTileRadius;
  s.selected = s.normal;
  s.selected.background = fui::Paint::solid(fui::Color::Black);
  s.selected.foreground = fui::Paint::solid(fui::Color::White);
  s.focused = s.selected;
  s.active = s.selected;
  s.disabled = s.normal;
  return s;
}

constexpr fui::StyleSet kStepStyles = makeStepStyles();
constexpr fui::StyleSet kTileStyles = makeTileStyles();
}  // namespace

// main.cpp's deep-sleep entry (persists state, draws the sleep screen, sleeps).
void enterDeepSleep(bool fromTimeout);

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
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
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      // Nothing else would turn the renderer: ActivityManager::Pop restores the
      // activity underneath without calling onEnter(), so its
      // applyInitialOrientation() never runs. Apply it here instead — renderUi()
      // re-derives the device context on the next render, so the hit rects
      // follow the new frame on their own. A rotation rewrites every pixel, so
      // it takes the clean waveform like night mode does.
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 3:  // Touch kill-switch (for reading with the palm on the glass)
      gpio.setTouchEnabled(!gpio.touchEnabled());
      requestUpdate();
      break;
    case 4: {  // Screenshot of what is on screen (panel included) to the SD card
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      break;
    }
    case 5:  // Sleep now
      // enterDeepSleep() does not return, so onExit() will not run: fold the
      // panel's own unsaved brightness/warmth in before it writes and sleeps,
      // or a slider moved just before tapping this tile is lost on wake.
      persistLightSettings();
      SETTINGS.saveToFile();
      enterDeepSleep(false);
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
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
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) draggingSlider = false;
    return;
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
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  int y = tokens.spaceLg + tokens.spaceMd;  // top padding
  if (Frontlight.present()) {
    y += lineHeight + tokens.spaceMd + kSliderRowHeight + tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + kSliderRowHeight + tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  // Only the tiles the user kept take up height, or the panel would hang down
  // over empty space (and, with every tile hidden, the sheet is exactly the
  // frontlight controls).
  int ids[kTileCount];
  const int tileCount = visibleTiles(mappedInput, ids);
  if (tileCount > 0) {
    const int rows = (tileCount + kTileCols - 1) / kTileCols;
    y += rows * kTileHeight + (rows - 1) * kTileGap;
  }
  y += tokens.spaceMd + kGrabberHeight + tokens.spaceLg;  // grabber band + air
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  const auto& theme = screen.theme();
  const fui::Insets side{0, kPanelSideMargin, 0, kPanelSideMargin};
  const int16_t lineHeight = screen.target().lineHeight(theme.smallText.font);

  // Caption line: name on the left, live percentage on the right. spaceMd below
  // it, not spaceXs: Vietnamese descenders (the dot under "Độ") reached into the
  // pill's outline at the tighter gap.
  const fui::Rect caption = screen.takeTop(lineHeight, theme.spaceMd).inset(side);
  fui::TextStyle nameStyle = theme.smallText;
  nameStyle.bold = true;
  screen.target().text(caption, label, nameStyle);
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));
  fui::TextStyle pctStyle = theme.smallText;
  pctStyle.align = fui::TextAlign::Right;
  screen.target().text(caption, pct, pctStyle);

  // [-] [=== pill ===] [+] (and a lamp toggle after the + on the brightness
  // row): explicit step buttons, because a drag on the etched matte glass is
  // unreliable and a hidden tap zone is worse than no zone at all. The pill
  // itself stays draggable / tap-to-jump.
  const fui::Rect band = screen.takeTop(kSliderRowHeight, theme.spaceMd).inset(side);
  const int16_t stepW = kSliderRowHeight;  // square, finger-sized
  const auto stepButton = [&](const int16_t x, const char* glyph, const int16_t delta) {
    const fui::Rect rect{x, band.y, stepW, kSliderRowHeight};
    stepProps.label = glyph;
    stepProps.action = stepAction;
    stepProps.value = delta;
    stepProps.inputMask = fui::InputTouch;
    // Title weight: a body-size "-" is a hairline inside a 56px button.
    stepProps.text = theme.titleText;
    stepProps.text.bold = true;
    stepProps.styles = kStepStyles;
    screen.button(stepProps, rect);
  };

  int16_t bandRight = band.right();
  if (showToggle) {
    // Lamp on/off, at the far right of the row: the sliders set the level, this
    // kills the light outright. Filled glyph = on, outline = off.
    const fui::Rect toggle{static_cast<int16_t>(bandRight - stepW), band.y, stepW, kSliderRowHeight};
    const fui::BitmapRef sunIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
    const int16_t iconW = static_cast<int16_t>(sunIcon.width);
    const int16_t iconH = static_cast<int16_t>(sunIcon.height);
    screen.frame().hit(toggle, ACTION_TOGGLE, 0, fui::InputTouch);
    screen.target().stroke(toggle, fui::Paint::solid(fui::Color::Black), 2, kTileRadius);
    screen.target().bitmap(fui::Rect{static_cast<int16_t>(toggle.x + (stepW - iconW) / 2),
                                     static_cast<int16_t>(toggle.y + (kSliderRowHeight - iconH) / 2), iconW, iconH},
                           sunIcon, fui::BitmapMode::Center);
    bandRight = static_cast<int16_t>(bandRight - stepW - theme.spaceMd);
  }

  stepButton(band.x, "-", -BRIGHTNESS_STEP);
  const int16_t plusX = static_cast<int16_t>(bandRight - stepW);
  stepButton(plusX, "+", BRIGHTNESS_STEP);
  // The pill spans the gap between the two step buttons.
  const int16_t rowX = static_cast<int16_t>(band.x + stepW + theme.spaceMd);
  const fui::Rect row{rowX, band.y, static_cast<int16_t>(plusX - theme.spaceMd - rowX), kSliderRowHeight};
  constexpr int16_t kStroke = 2;

  // 1-bit capsule, drawn here rather than through fui::slider: that component
  // pairs a square-cornered progress fill with a separate knob, which on a
  // capsule this tall broke the outline at the left end and put a black knob
  // inside a black fill (invisible, with a notch where the two met). A plain
  // filled capsule is the iOS brightness control anyway — the fill edge IS the
  // handle. Drag still works identically: dragPermille comes from the hit rect,
  // so registering it directly gives the same events fui::slider would.
  // Two fixed 56px step buttons, an optional 56px lamp toggle and the gaps
  // between them are charged against the row before the pill gets what is left.
  // Below the width of the handle itself there is no capsule to draw — the
  // handle would spill over the buttons either side, and a track thinner than
  // the 2px outline would put a negative width into fill(). The step buttons
  // still drive the value there, so the row stays usable.
  const int16_t minTrack = static_cast<int16_t>(kSliderRowHeight + 2 * kStroke);
  if (row.width < minTrack) return;

  screen.frame().hit(row, sliderAction, 0, fui::InputTouch | fui::InputDrag);
  screen.target().fill(row, fui::Paint::solid(fui::Color::White), kSliderTrackRadius);
  // The fill sits INSIDE the outline (inset by the stroke width) so the capsule
  // reads as one unbroken shape at every value instead of the fill riding over
  // its own border.
  const fui::Rect inner = row.inset(fui::Insets{kStroke, kStroke, kStroke, kStroke});
  // A round handle rides the boundary between the filled and empty track, and
  // the fill runs to its center. The handle is what hides the fill's square
  // right edge — the earlier attempts either showed that edge or, with all four
  // corners rounded, needed a whole-stadium minimum width that made every value
  // under ~20% draw identically. Drawn white with its own outline so it stays
  // visible against the black fill on one side and the white track on the other.
  const int16_t cap = static_cast<int16_t>(inner.height / 2);
  const int16_t travel = static_cast<int16_t>(inner.width - 2 * cap);
  const int16_t handleCx = static_cast<int16_t>(inner.x + cap + (travel * value) / 100);
  {
    // The fill runs to the handle's far edge as a full stadium, so its right cap
    // is a semicircle about the handle's own center and radius — entirely under
    // the handle, touching the capsule only where the handle does. Ending the
    // fill at the handle's center instead (or squaring its right edge) left its
    // top and bottom corners poking out past the round handle: the black
    // rectangle either side of it.
    const int16_t w = static_cast<int16_t>(std::min<int>(inner.width, handleCx + cap - inner.x));
    screen.target().fill(fui::Rect{inner.x, inner.y, w, inner.height}, fui::Paint::solid(fui::Color::Black), cap);
  }
  screen.target().stroke(row, fui::Paint::solid(fui::Color::Black), kStroke, kSliderTrackRadius);
  const fui::Rect handle{static_cast<int16_t>(handleCx - cap), inner.y, static_cast<int16_t>(cap * 2), inner.height};
  screen.target().fill(handle, fui::Paint::solid(fui::Color::White), cap);
  screen.target().stroke(handle, fui::Paint::solid(fui::Color::Black), kStroke, cap);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - panelBottom);
  // No header: the panel is a floating card, and its own grabber says what it
  // is. (A title band here just repeated the obvious.)
  screen.setContentMargin(fui::Insets{0, 0, bottomInset, 0});

  // The sheet hangs from the very top of the panel, so it needs a real top inset
  // of its own — nothing above it reserves space the way a header band did.
  screen.spacer(static_cast<int16_t>(theme.spaceLg + theme.spaceMd));

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/true);
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  // Quick-setting tiles. Two columns of finger-sized cards; a tile whose
  // setting is currently on draws filled (StateChecked -> selected style). Only
  // the tiles the user kept are laid out, and the button's value stays the tile
  // id (not its grid position) so runTile() is unaffected by what is hidden.
  static_assert(kTileCount == 6, "visibleTiles() writes up to six tile ids");
  int ids[kTileCount];
  const int tileCount = visibleTiles(mappedInput, ids);
  if (tileCount > 0) {
    static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                              StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    char orientLabel[64];
    snprintf(orientLabel, sizeof(orientLabel), "%s: %s", tr(STR_ORIENTATION),
             I18N.get(kOrientNames[SETTINGS.orientation % 4]));

    const char* labels[kTileCount] = {tr(STR_NIGHT_MODE),   tr(STR_FORCE_REFRESH),     orientLabel,
                                      tr(STR_TOUCH_TOGGLE), tr(STR_SCREENSHOT_BUTTON), tr(STR_SLEEP)};
    const fui::State states[kTileCount] = {
        SETTINGS.screenInverted ? fui::StateChecked : fui::StateNormal, fui::StateNormal, fui::StateNormal,
        // "on" here means touch is DISABLED — the filled tile marks the
        // non-default, attention-worthy state (input is off).
        gpio.touchEnabled() ? fui::StateNormal : fui::StateChecked, fui::StateNormal, fui::StateNormal};

    const int rows = (tileCount + kTileCols - 1) / kTileCols;
    for (int r = 0; r < rows; ++r) {
      const fui::Rect band = screen.takeTop(kTileHeight, r + 1 < rows ? kTileGap : 0)
                                 .inset(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});
      const int16_t tileW = static_cast<int16_t>((band.width - kTileGap * (kTileCols - 1)) / kTileCols);
      for (int c = 0; c < kTileCols; ++c) {
        const int slot = r * kTileCols + c;
        if (slot >= tileCount) break;
        const int id = ids[slot];
        const fui::Rect tile{static_cast<int16_t>(band.x + c * (tileW + kTileGap)), band.y, tileW, kTileHeight};
        tileProps.label = labels[id];
        tileProps.action = ACTION_TILE;
        tileProps.value = static_cast<int16_t>(id);
        tileProps.state = states[id];
        tileProps.styles = kTileStyles;
        tileProps.text = theme.smallText;
        screen.button(tileProps, tile);
      }
    }
  }

  // Grabber last: the handle belongs on the edge the sheet is dragged from, and
  // this sheet hangs from the top of the screen, so it sits along the bottom.
  {
    screen.spacer(theme.spaceMd);
    const fui::Rect band = screen.takeTop(kGrabberHeight, theme.spaceLg);
    const fui::Rect grabber{static_cast<int16_t>(band.x + (band.width - kGrabberWidth) / 2), band.y, kGrabberWidth,
                            kGrabberHeight};
    screen.target().fill(grabber, fui::Paint::solid(fui::Color::Black), kGrabberHeight / 2);
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();
  const int pageWidth = renderer.getScreenWidth();

  // Card: white body, a 2px rule along its bottom edge, rounded bottom corners
  // so it reads as a sheet pulled down over the page behind it.
  renderer.fillRect(0, 0, pageWidth, panelBottom, false);

  renderUi();

  renderer.fillRect(0, panelBottom - 2, pageWidth, 2, true);
  // A tile that rewrote the whole frame (night mode) or explicitly asked for a
  // cleanup re-drives every pixel once; ordinary repaints stay on the fast path.
  // FULL, not HALF: HALF is the balanced-speed waveform and leaves some ghost
  // behind, which is exactly what the tile that sets this flag is asked to
  // remove.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
