#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32C3 || CROSSPOINT_NATIVE_DFU
#include <HalPowerManager.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>

#include "UsbDfuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/UsbDfuService.h"

void UsbDfuActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_OFF);
  powerManager.setPowerSaving(false);
  requestUpdateAndWait();
  usb_dfu::allowTransfer();
}
void UsbDfuActivity::onExit() {
  renderer.waitRefreshComplete();
  usb_dfu::releaseScreen();
  Activity::onExit();
}
bool UsbDfuActivity::requiresExclusiveStorageLoop() const {
  const auto state = usb_dfu::snapshot();
  return state.running && state.stage != usb_dfu::Stage::Waiting;
}
void UsbDfuActivity::loop() {
  if (finished) return;
  const auto state = usb_dfu::snapshot();
  if (!state.running && state.installed) {
    finished = true;
    requestUpdateAndWait();
    delay(1000);
    ESP.restart();
  } else if (!state.running || state.stage == usb_dfu::Stage::Waiting) {
    finished = true;
    activityManager.goHome();
  }
}
void UsbDfuActivity::render(RenderLock&&) {
  const auto state = usb_dfu::snapshot();
  renderer.waitRefreshComplete();
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_USB_FIRMWARE_UPDATE));
  const int lineHeight = std::max(renderer.getLineHeight(UI_10_FONT_ID), renderer.getTextHeight(UI_10_FONT_ID));
  int insetTop, insetRight, insetBottom, insetLeft;
  renderer.getOrientedViewableTRBL(&insetTop, &insetRight, &insetBottom, &insetLeft);
  const int contentTop = std::max(insetTop, metrics.topPadding + metrics.headerHeight) + metrics.verticalSpacing;
  const int contentBottom = height - insetBottom - metrics.verticalSpacing;
  const int gap = std::max(8, metrics.verticalSpacing);
  const int blockHeight = 2 * lineHeight + gap;
  const int y = contentTop + std::max(0, (contentBottom - contentTop - blockHeight) / 2);
  const char* text = state.installed                            ? tr(STR_UPDATE_COMPLETE)
                     : state.stage == usb_dfu::Stage::Verifying ? tr(STR_VALIDATING_FIRMWARE)
                                                                : tr(STR_USB_DFU_RECEIVING);
  renderer.drawCenteredText(UI_10_FONT_ID, y, text, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight + gap, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  renderer.displayBuffer();
}
#endif
