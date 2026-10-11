#include "NetworkModeSelectionActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <iterator>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/icons/networkModeIcons.h"

namespace fui = freeink::ui;

namespace {
// The last row is the USB transfer method this board actually has. Boards
// that can act as a USB mass-storage device (FREEINK_CAP_USB_MSC) show "USB
// Drive"; the rest keep the serial protocol.
#if FREEINK_CAP_USB_MSC
constexpr NetworkMode USB_MODE = NetworkMode::USB_DRIVE;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_DRIVE;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_DRIVE_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconUsb;
#else
constexpr NetworkMode USB_MODE = NetworkMode::USB_SERIAL;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_TRANSFER;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_TRANSFER_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconTransfer;
#endif

struct ModeRow {
  NetworkMode mode;
  StrId label;
  StrId desc;
  const uint8_t* icon;
};

// Display order, top to bottom.
constexpr ModeRow kRows[] = {
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, networkModeIconWifi},
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, networkModeIconLibrary},
    {NetworkMode::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_DESC, networkModeIconOpds},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, networkModeIconHotspot},
    {USB_MODE, USB_MODE_LABEL, USB_MODE_DESC, USB_MODE_ICON},
};
static_assert(std::size(kRows) == NetworkModeSelectionActivity::MAX_ROWS);

// The OPDS browser has nothing to open until a server is configured (Settings > System > OPDS
// Servers, or the web settings page).
bool isListed(const ModeRow& row) { return row.mode != NetworkMode::OPDS_BROWSER || OPDS_STORE.hasServers(); }

// A 32 px icon from networkModeIcons.h as the bitmap a list row draws (Mask1: bit 0 = ink).
fui::BitmapRef rowIcon(const uint8_t* bits) {
  fui::BitmapRef icon;
  icon.data = bits;
  icon.width = 32;
  icon.height = 32;
  icon.format = fui::BitmapFormat::Mask1;
  return icon;
}
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const NetworkMode selected)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput), selectedOnEnter(selected) {
  for (const ModeRow& row : kRows) {
    if (!isListed(row)) continue;
    fui::ListItem item;
    item.label = I18N.get(row.label);
    item.subtitle = I18N.get(row.desc);
    item.icon = rowIcon(row.icon);
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowModes[rowCount] = row.mode;
    rowCount++;
  }
}

void NetworkModeSelectionActivity::onEnter() {
  // The base resets the selection to the top row; move it after.
  UiListActivity::onEnter();
  for (int i = 1; i < rowCount; i++) {
    if (rowModes[i] == selectedOnEnter) {
      moveSelectionTo(i);
      break;
    }
  }
}

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Every choice leaves this screen; a lingering tap flash would gray an unrelated element on the
  // next screen's first render.
  app.clearTapFlash();
  nav.selected = index;
  const NetworkMode mode = rowModes[index];

  // Neither USB mode needs WiFi or the web server, so hand off directly here
  // instead of routing the result back through the WiFi-centric
  // CrossPointWebServerActivity. The OPDS browser does need WiFi, but brings it up
  // itself and reboots on exit, so it is handed off the same way. The web server
  // modes still return to that owner.
  if (mode == NetworkMode::OPDS_BROWSER) {
    activityManager.goToBrowser();
    return;
  }
  if (mode == NetworkMode::USB_SERIAL) {
    activityManager.goToSerialTransfer();
    return;
  }
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
  onModeSelected(mode);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  addList(screen, props, /*hasSubtitle=*/true);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
