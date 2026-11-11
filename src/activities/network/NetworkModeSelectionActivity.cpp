#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
struct ModeRow {
  NetworkMode mode;
  StrId label;
  StrId desc;
  UIIcon icon;
};

constexpr ModeRow MODE_ROWS[] = {
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, UIIcon::Wifi},
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, UIIcon::Library},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, UIIcon::Hotspot},
    {NetworkMode::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_DESC, UIIcon::Book},
#if FREEINK_CAP_USB_MSC
    {NetworkMode::USB_DRIVE, StrId::STR_USB_DRIVE, StrId::STR_USB_DRIVE_DESC, UIIcon::Usb},
#endif
};
static_assert(sizeof(MODE_ROWS) / sizeof(MODE_ROWS[0]) == NetworkModeSelectionActivity::MAX_ITEM_COUNT);
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Home shows OPDS in its Plugins slot while plugins are off and a server
  // exists, so the row is listed here only in the other cases.
  const bool opdsOnHome = !SETTINGS.pluginsEnabled && OPDS_STORE.hasServers();
  // Static for the screen's lifetime, so built once here rather than every
  // buildScreen() call.
  for (const ModeRow& row : MODE_ROWS) {
    if (row.mode == NetworkMode::OPDS_BROWSER && opdsOnHome) continue;
    fui::ListItem item;
    item.label = I18N.get(row.label);
    item.subtitle = I18N.get(row.desc);
    item.icon = listIconFor(row.icon, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(rowCount_);
    rowModes_[rowCount_] = row.mode;
    rowItems_[rowCount_++] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return rowCount_; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  onModeSelected(rowModes_[index]);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(rowCount_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
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
