#pragma once

#include <GfxRenderer.h>

#include "activities/UiListActivity.h"
#include "activities/util/KeyboardLayoutSet.h"

class MappedInputManager;

/**
 * Picks which keyboard layouts the language key cycles through.
 *
 * Every layout the SDK ships is listed; Confirm toggles one on or off. The set
 * is a bit mask in settings, so enabling a layout costs no RAM -- the tables are
 * const and sit in flash either way.
 */
class KeyboardLayoutsActivity final : public UiListActivity {
 public:
  explicit KeyboardLayoutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("KeyboardLayouts", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return totalItems; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  static constexpr uint8_t totalItems = keyboard_layouts::COUNT;

  // Row storage: totalItems is a compile-time constant, so a fixed-capacity
  // array avoids any heap allocation for the row list. Labels are set once in
  // onEnter(); only the ON/OFF value text is refreshed per build, and both
  // texts are I18n table pointers, so no string storage is needed.
  freeink::ui::ListItem rowItems[totalItems]{};

  // Working copy: the screen edits this and writes it back on exit, so a single
  // settings write covers a whole editing session rather than one per keypress.
  uint16_t workingMask = 0;
  // Whether the user actually toggled anything. Without this, merely opening the
  // screen on an unconfigured device would persist the derived default as an
  // explicit choice -- costing a settings write for nothing, and freezing the set
  // so a later UI language change no longer brings its layout along.
  bool edited = false;
};
