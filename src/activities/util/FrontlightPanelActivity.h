#pragma once

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored control center opened by a top-edge down-swipe, a status-bar tap,
// or a button bound to "Control Center" (iOS Control Center style): a grabber,
// the frontlight brightness/warmth sliders (on boards with a light), and a grid
// of quick-setting tiles — night mode, ghost-cleanup refresh, reading
// orientation, the touch kill-switch, a screenshot, and sleep, each of which the
// user can hide (Settings -> Display -> Customise Control Center). The
// frontlight controls are always there: they are what the panel is for. Pure
// 1-bit: no dithered fills, selection reads as a filled tile. The grabber sits
// along the panel's bottom edge, the edge the sheet is dragged from.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  int panelBottom = 0;

  // fui::ButtonProps embeds a 324-byte fui::StyleSet, so the two the render
  // path fills in live here instead of on the stack (AGENTS.md: locals stay
  // under 256 bytes). ui::button() takes them by const reference and draws
  // immediately, so one instance per call site is enough — every field either
  // is reassigned on each use or keeps its constructed default.
  freeink::ui::ButtonProps stepProps;
  freeink::ui::ButtonProps tileProps;

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // One slider row: a caption line (name + live percentage) above
  // [-] [draggable 1-bit capsule] [+], plus a lamp on/off button after the +
  // when showToggle is set (the brightness row).
  void addSliderRow(UiScreen& screen, const char* label, uint8_t value, freeink::ui::ActionId sliderAction,
                    freeink::ui::ActionId stepAction, bool showToggle);
  int computePanelBottom() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on the ordinary way out; the
  // sleep tile has to run it itself, because enterDeepSleep() never returns.
  void persistLightSettings();
  void close();

  // Quick-setting tiles, in grid order (2 columns). This is the catalogue size;
  // which of them are laid out is a user setting (Settings -> Display ->
  // Customise Control Center), so the grid may be shorter or empty.
  static constexpr int kTileCount = 6;
  // One-shot: the "refresh" tile re-drives the whole frame with the
  // ghost-cleanup waveform on the next render.
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
};
