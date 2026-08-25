#include "BluetoothSettingsActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "BleButtonMapActivity.h"
#include "BleInput.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long kBannerMs = 2000;
constexpr uint32_t kScanMs = 8000;
constexpr unsigned long kForgetHoldMs = 1200;  // hold Confirm this long in the Paired view to forget
}  // namespace

void BluetoothSettingsActivity::onEnter() {
  view = View::Menu;
  UiListActivity::onEnter();
}

void BluetoothSettingsActivity::onExit() {
  if (BleHid.isScanning()) BleHid.stopScan();
  Activity::onExit();
}

void BluetoothSettingsActivity::setBanner(const char* text) {
  banner = text ? text : "";
  bannerUntil = millis() + kBannerMs;
}

void BluetoothSettingsActivity::rebuildRows() {
  rowCount = 0;
  const auto addRow = [this](const char* label, const int16_t value) -> fui::ListItem* {
    if (rowCount >= MAX_ROWS) return nullptr;
    rowItems[rowCount] = fui::ListItem{};
    rowItems[rowCount].label = label;
    rowItems[rowCount].actionValue = static_cast<int16_t>(rowCount);
    rowValues[rowCount] = value;
    return &rowItems[rowCount++];
  };

  if (view == View::Menu) {
    if (auto* row = addRow(tr(STR_BLUETOOTH), static_cast<int16_t>(Action::ToggleBt))) {
      row->toggle = true;
      row->toggleChecked = SETTINGS.bluetoothEnabled != 0;
    }
    if (SETTINGS.bluetoothEnabled) {
      addRow(tr(STR_BT_SCAN_PAIR), static_cast<int16_t>(Action::Scan));
      if (BleHid.isConnected()) {
        if (auto* row = addRow(tr(STR_BT_DISCONNECT), static_cast<int16_t>(Action::Disconnect))) {
          row->value = BleHid.connectedName();
        }
      }
      addRow(tr(STR_BT_PAIRED_DEVICES), static_cast<int16_t>(Action::PairedDevices));
      addRow(tr(STR_BT_MAP_BUTTONS), static_cast<int16_t>(Action::MapButtons));
    }
  } else if (view == View::Scan) {
    const int count = BleHid.deviceCount();
    for (int i = 0; i < count; i++) {
      addRow(BleHid.device(static_cast<uint8_t>(i)).name, static_cast<int16_t>(i));
    }
    if (count == 0) {
      if (BleHid.isScanning()) {
        if (auto* row = addRow(tr(STR_SCANNING), -1)) row->enabled = false;
      } else {
        // Scan finished with nothing found: one actionable row to scan again.
        addRow(tr(STR_BT_NO_DEVICES), -1);
      }
    }
  } else {  // Paired
    const int count = BleHid.pairedCount();
    for (int i = 0; i < count; i++) {
      addRow(BleHid.paired(static_cast<uint8_t>(i)).name, static_cast<int16_t>(i));
    }
    if (count == 0) {
      if (auto* row = addRow(tr(STR_BT_NO_PAIRED), -1)) row->enabled = false;
    }
  }
}

void BluetoothSettingsActivity::enterScanView() {
  LOG_INF("BLEUI", "scan view: begin running=%d scanning=%d devices=%u paired=%u", BleHid.isRunning(),
          BleHid.isScanning(), BleHid.deviceCount(), BleHid.pairedCount());
  view = View::Scan;
  awaitingConnect = false;
  lastLoggedScanState = false;
  lastLoggedDeviceCount = 0xFF;
  // The main-loop lifecycle owns steady-state start/stop, but a scan needs the stack
  // this instant — entering this screen can precede the lifecycle's next tick, or its
  // heap gate may have deferred the start. ensureStarted() is idempotent, and with
  // this screen on top the lifecycle keeps the stack up (keepsBluetoothAlive).
  if (!BleHid.isRunning() && !bleinput::ensureStarted()) {
    LOG_ERR("BLEUI", "scan: BLE start failed (heap=%u)", ESP.getFreeHeap());
  }
  BleHid.startScan(kScanMs);
  LOG_INF("BLEUI", "scan view: startScan requested scanning=%d devices=%u", BleHid.isScanning(), BleHid.deviceCount());
  moveSelectionTo(0);
}

void BluetoothSettingsActivity::connectTo(const char* addr) {
  awaitingConnect = true;
  setBanner(tr(STR_CONNECTING));
  BleHid.connect(addr);
  requestUpdate();
}

void BluetoothSettingsActivity::forgetPaired(const int pairedIndex) {
  if (pairedIndex < 0 || pairedIndex >= BleHid.pairedCount()) return;
  const auto& p = BleHid.paired(static_cast<uint8_t>(pairedIndex));
  BleHid.forget(p.addr);
  setBanner(tr(STR_FORGET_BUTTON));
  moveSelectionTo(0);
}

void BluetoothSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount) return;
  app.clearTapFlash();
  const int16_t value = rowValues[index];

  if (view == View::Menu) {
    switch (static_cast<Action>(value)) {
      case Action::ToggleBt:
        // Flip the preference only; the main-loop lifecycle check starts/stops the BLE
        // stack to match. Single owner.
        SETTINGS.bluetoothEnabled = SETTINGS.bluetoothEnabled ? 0 : 1;
        SETTINGS.saveToFile();
        requestUpdate();
        break;
      case Action::Scan:
        enterScanView();
        break;
      case Action::Disconnect:
        BleHid.disconnect();
        setBanner(tr(STR_BT_NOT_CONNECTED));
        requestUpdate();
        break;
      case Action::PairedDevices:
        view = View::Paired;
        moveSelectionTo(0);
        break;
      case Action::MapButtons:
        startActivityForResult(std::make_unique<BleButtonMapActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      default:
        break;
    }
    return;
  }

  if (view == View::Scan) {
    if (value < 0) {
      // The "no devices" row: scan again.
      if (!awaitingConnect && !BleHid.isScanning()) {
        LOG_INF("BLEUI", "scan view: restart scan requested");
        if (!BleHid.isRunning() && !bleinput::ensureStarted()) {
          LOG_ERR("BLEUI", "scan restart: BLE start failed (heap=%u)", ESP.getFreeHeap());
        }
        BleHid.startScan(kScanMs);
        requestUpdate();
      }
      return;
    }
    if (!awaitingConnect && value < BleHid.deviceCount()) {
      if (BleHid.isScanning()) BleHid.stopScan();
      const auto& d = BleHid.device(static_cast<uint8_t>(value));
      LOG_INF("BLEUI", "scan view: connect addr=%s name='%s' rssi=%d type=%u hid=%d conn=%d", d.addr, d.name, d.rssi,
              d.addrType, d.hid, d.connectable);
      connectTo(d.addr);
    }
    return;
  }

  // Paired: tap connects; long-press / Confirm hold forgets (see onRowLongPress /
  // handleButtons).
  if (!awaitingConnect && value >= 0 && value < BleHid.pairedCount()) {
    connectTo(BleHid.paired(static_cast<uint8_t>(value)).addr);
  }
}

void BluetoothSettingsActivity::onRowLongPress(const int index) {
  if (view != View::Paired || index < 0 || index >= rowCount) return;
  app.clearTapFlash();
  forgetPaired(rowValues[index]);
}

void BluetoothSettingsActivity::onBackButton() {
  if (view == View::Menu) {
    finish();
    return;
  }
  if (BleHid.isScanning()) BleHid.stopScan();
  view = View::Menu;
  moveSelectionTo(0);
}

bool BluetoothSettingsActivity::handleButtons() {
  // Paired view: hold Confirm to forget the selected device; release connects.
  // Uses release for connect so a hold can fire forget without also connecting
  // on the same press.
  if (view == View::Paired) {
    if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      if (!pairedHoldActionTaken && mappedInput.getHeldTime() >= kForgetHoldMs && nav.selected >= 0 &&
          nav.selected < rowCount) {
        forgetPaired(rowValues[nav.selected]);
        pairedHoldActionTaken = true;
      }
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const bool consumed = pairedHoldActionTaken;
      pairedHoldActionTaken = false;
      if (!consumed && nav.selected >= 0 && nav.selected < rowCount) activateIndex(nav.selected);
      return true;
    }
  }
  return UiListActivity::handleButtons();
}

bool BluetoothSettingsActivity::handleCustomInput() {
  // Clear an expired status banner.
  if (bannerUntil > 0 && millis() > bannerUntil) {
    banner.clear();
    bannerUntil = 0;
    requestUpdate();
  }

  // Watch for an async connect result (from either the scan list or the paired list).
  if (awaitingConnect) {
    char reason[48];
    if (BleHid.isConnected()) {
      awaitingConnect = false;
      BleHid.releaseScanResults();
      view = View::Menu;
      char buf[64];
      snprintf(buf, sizeof(buf), tr(STR_BT_CONNECTED_TO), BleHid.connectedName());
      setBanner(buf);
      moveSelectionTo(0);
    } else if (BleHid.takeConnectFailure(reason, sizeof(reason))) {
      awaitingConnect = false;
      setBanner(reason);
      requestUpdate();
    }
  }

  // The scan list changes as devices are discovered — repaint on state changes.
  if (view == View::Scan) {
    const bool scanning = BleHid.isScanning();
    const uint8_t deviceCount = BleHid.deviceCount();
    if (scanning != lastLoggedScanState || deviceCount != lastLoggedDeviceCount) {
      LOG_INF("BLEUI", "scan view: state scanning=%d devices=%u", scanning, deviceCount);
      lastLoggedScanState = scanning;
      lastLoggedDeviceCount = deviceCount;
    }
    if (scanning != renderedScanning || deviceCount != renderedDeviceCount) requestUpdate();
  }
  return false;
}

void BluetoothSettingsActivity::drawChrome() {
  UiListActivity::drawChrome();
  // Sub-header: transient banner when set, else the live connection status.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* status = !banner.empty()        ? banner.c_str()
                       : BleHid.isConnected() ? BleHid.connectedName()
                                              : tr(STR_BT_NOT_CONNECTED);
  GUI.drawSubHeader(renderer,
                    Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight},
                    status);
}

void BluetoothSettingsActivity::buildScreen(UiScreen& screen) {
  rebuildRows();
  renderedScanning = BleHid.isScanning();
  renderedDeviceCount = BleHid.deviceCount();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header + status sub-header bands drawChrome paints.
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;  // physical buttons stay in loop()
  // Master-switch geometry: larger than the list default so it reads as the
  // primary control, inset from the row edge, and rounded with the same theme
  // shape token the buttons and the frontlight panel's sliders inherit.
  const auto& theme = screen.theme();
  props.toggleWidth = mappedInput.hasTouch() ? 52 : 44;
  props.toggleHeight = mappedInput.hasTouch() ? 26 : 22;
  props.toggleKnobInset = 4;
  props.toggleRadius = theme.controlRadius;      // clamped to a capsule by the renderer
  props.toggleKnobRadius = theme.controlRadius;  // ditto for the knob
  props.valueInset = theme.spaceMd;              // air between switch/value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}
