#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
#include "LocalSendReceiveActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::BASE_MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::BASE_MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::BASE_MENU_ITEM_COUNT] = {
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < BASE_MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
  if (LocalSendReceiveActivity::isAvailable()) {
    fui::ListItem item;
    item.label = "Local Send";
    item.subtitle = "Receive files from nearby devices";
    item.icon = listIconFor(UIIcon::Wifi, 32);
    item.actionValue = BASE_MENU_ITEM_COUNT;
    rowItems_[BASE_MENU_ITEM_COUNT] = item;
    visibleItems_ = MENU_ITEM_COUNT;
  }
#endif
}

int NetworkModeSelectionActivity::listCount() const { return visibleItems_; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  app.clearTapFlash();
  nav.selected = index;
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
  if (index == BASE_MENU_ITEM_COUNT && visibleItems_ == MENU_ITEM_COUNT) {
    onModeSelected(NetworkMode::LOCAL_SEND);
    return;
  }
#endif
  onModeSelected(static_cast<NetworkMode>(index));
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
  props.count = static_cast<uint16_t>(visibleItems_);
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
