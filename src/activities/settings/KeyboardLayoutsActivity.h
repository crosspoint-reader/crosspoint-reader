#pragma once

#include <GfxRenderer.h>

#include "activities/Activity.h"
#include "activities/util/KeyboardLayoutSet.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Picks which keyboard layouts the language key cycles through.
 *
 * Every layout the SDK ships is listed; Confirm toggles one on or off. The set
 * is a bit mask in settings, so enabling a layout costs no RAM -- the tables are
 * const and sit in flash either way.
 */
class KeyboardLayoutsActivity final : public Activity {
 public:
  explicit KeyboardLayoutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KeyboardLayouts", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void toggleSelected();
  void onBack() { finish(); }

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  // Working copy: the screen edits this and writes it back on exit, so a single
  // settings write covers a whole editing session rather than one per keypress.
  uint16_t workingMask = 0;
  // Whether the user actually toggled anything. Without this, merely opening the
  // screen on an unconfigured device would persist the derived default as an
  // explicit choice -- costing a SPIFFS write for nothing, and freezing the set
  // so a later UI language change no longer brings its layout along.
  bool edited = false;
  static constexpr uint8_t totalItems = keyboard_layouts::COUNT;
};
