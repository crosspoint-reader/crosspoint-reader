#pragma once

#include <GfxRenderer.h>

#include "activities/UiListActivity.h"
#include "activities/util/KeyboardLayoutSet.h"

class MappedInputManager;

/**
 * Picks which keyboard layouts the language key cycles through.
 *
 * Every layout the SDK ships is listed; Confirm toggles one on or off. Layout
 * tables are const and sit in flash either way, so enabling one costs no RAM.
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

  // Whether row `i` is on and cannot be switched off, because the set would end
  // up empty or without a Latin layout.
  bool isLocked(uint8_t i) const;

  static constexpr uint8_t totalItems = keyboard_layouts::COUNT;

  // Fixed capacity: totalItems is a compile-time constant, so the row list
  // needs no heap. Both texts are I18n table pointers, so no string storage
  // either.
  freeink::ui::ListItem rowItems[totalItems]{};

  // Written back on exit, so one settings write covers a whole editing session.
  uint16_t workingMask = 0;
  // Without this, merely visiting the screen would persist the derived default
  // as an explicit choice and freeze the set against a later UI language change.
  bool edited = false;
};
