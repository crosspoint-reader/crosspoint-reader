#pragma once

#include <GfxRenderer.h>

#include "activities/UiListActivity.h"
#include "activities/util/KeyboardLayoutSet.h"

class MappedInputManager;

class KeyboardLayoutsActivity final : public UiListActivity {
 public:
  explicit KeyboardLayoutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("KeyboardLayouts", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return keyboard_layouts::COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  bool isLocked(uint8_t i) const;

  freeink::ui::ListItem rowItems[keyboard_layouts::COUNT]{};

  uint16_t workingMask = 0;
  // Do not persist a derived default merely because the screen was visited.
  bool edited = false;
};
