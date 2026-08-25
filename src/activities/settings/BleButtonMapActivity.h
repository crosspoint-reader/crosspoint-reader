#pragma once

#include <I18n.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/UiListActivity.h"

// Target-first mapping for BLE page-turner buttons, on a FreeInkUI list. The
// screen lists the logical functions (Page Forward, Page Back, Confirm, Back,
// directions) with the remote key currently bound to each. Activating a row —
// by touch tap or Confirm — enters capture mode for that function: the next
// key pressed on the remote binds to it (via the MappedInputManager BLE
// capture mode). No physical device button is required to pick the target, so
// the flow works on touch-only hardware too. Back cancels a capture.
class BleButtonMapActivity final : public UiListActivity {
 public:
  explicit BleButtonMapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("BleButtonMap", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  // Capturing needs the BLE stack up to receive the remote's key press.
  bool keepsBluetoothAlive() const override { return true; }

 private:
  // Logical functions a remote button can be bound to.
  struct Fn {
    MappedInputManager::Button button;
    StrId label;
  };
  static const Fn kFunctions[];
  static const uint8_t kFunctionCount;

  // >= 0: capture mode is armed for kFunctions[captureTarget].
  int captureTarget = -1;

  freeink::ui::ListItem rowItems[16]{};
  // Bound-key names shown in the row value slot ("Not set" when unbound).
  char valueBuf[16][24] = {};

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override { return tr(STR_BT_MAP_BUTTONS); }
  void drawChrome() override;

  void rebuildRows();
  void beginCapture(int index);
  void endCapture();
  // Bind the captured key to the chosen logical button in SETTINGS.bleKeyMap and
  // persist. Returns false when the table is full and the key is new.
  bool assignKey(MappedInputManager::Button button, uint8_t kind, uint8_t value);
};
