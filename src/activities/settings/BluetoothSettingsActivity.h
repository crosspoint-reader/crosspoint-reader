#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

// Bluetooth page-turner settings on a FreeInkUI list. One screen with three views:
//   Menu   — enable/disable BT (toggle row), scan & pair, paired devices, map buttons.
//   Scan   — live list of discovered BLE HID devices; activating a row connects.
//   Paired — bonded devices; tap connects, touch long-press / Confirm hold forgets.
// All BLE access goes through the FreeInk BleHid singleton; everything no-ops
// gracefully when BLE is compiled out (BleHid.begin() returns false).
class BluetoothSettingsActivity final : public UiListActivity {
 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("BluetoothSettings", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

  void onEnter() override;
  void onExit() override;
  bool keepsBluetoothAlive() const override { return true; }

 private:
  enum class View { Menu, Scan, Paired };

  // Menu row actions.
  enum class Action { ToggleBt, Scan, PairedDevices, MapButtons, Disconnect, RestartScan };

  // Rows are rebuilt from live BLE state on every buildScreen() pass; the scan
  // list grows while scanning. Fixed capacity keeps the row list off the heap.
  static constexpr int MAX_ROWS = 24;

  View view = View::Menu;
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  // Per-row action (Menu view) or device/paired index (Scan/Paired views).
  int16_t rowValues[MAX_ROWS] = {};
  int rowCount = 0;

  // Transient status banner (connect result, forget confirmation, etc.);
  // shown in the sub-header slot in place of the connection status.
  std::string banner;
  unsigned long bannerUntil = 0;

  // Set when a connect() has been issued and we're waiting for the async result.
  bool awaitingConnect = false;
  // Guards the Paired view's hold-to-forget so it fires once per hold and
  // suppresses the tap-to-connect on the same press.
  bool pairedHoldActionTaken = false;
  bool lastLoggedScanState = false;
  uint8_t lastLoggedDeviceCount = 0xFF;
  // Scan/paired state the last render used; loop() watches for changes and
  // repaints so discovered devices appear as they arrive.
  uint8_t renderedDeviceCount = 0xFF;
  bool renderedScanning = false;

  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  const char* headerTitle() const override { return tr(STR_BLUETOOTH); }
  void drawChrome() override;

  void rebuildRows();
  void enterScanView();
  void connectTo(const char* addr);
  void forgetPaired(int pairedIndex);
  void setBanner(const char* text);
};
