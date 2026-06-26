#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Bluetooth page-turner settings. One screen with three views:
//   Menu   — enable/disable BT, scan & pair, disconnect, map buttons, presets.
//   Scan   — live list of discovered BLE HID devices; Confirm connects.
//   Paired — bonded devices; Confirm forgets the selected one.
// All BLE access goes through the FreeInk BleHid singleton; everything no-ops
// gracefully when BLE is compiled out (BleHid.begin() returns false).
class BluetoothSettingsActivity final : public Activity {
 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool keepsBluetoothAlive() const override { return true; }

 private:
  enum class View { Menu, Scan, Paired };

  // Menu row actions.
  enum class Action { ToggleBt, Scan, Disconnect, MapButtons, PairedDevices };
  struct MenuRow {
    Action action;
    StrId label;
  };

  View view = View::Menu;
  std::vector<MenuRow> menuRows;
  int menuIndex = 0;
  int scanIndex = 0;
  int pairedIndex = 0;

  ButtonNavigator buttonNavigator;

  // Transient status banner (connect result, forget confirmation, etc.).
  std::string banner;
  unsigned long bannerUntil = 0;

  // Set when a connect() has been issued and we're waiting for the async result.
  bool awaitingConnect = false;
  // Guards the Paired view's hold-to-forget so it fires once per hold and suppresses
  // the tap-to-connect on the same press.
  bool pairedActionTaken = false;

  void rebuildMenuRows();
  void handleMenuConfirm();
  void startScanView();
  void setBanner(const char* text);

  std::string deviceLabel(int index) const;  // scan list row text
  std::string pairedLabel(int index) const;  // paired list row text
};
