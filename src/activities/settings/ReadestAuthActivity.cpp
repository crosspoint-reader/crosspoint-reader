#include "ReadestAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "ReadestSyncClient.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WifiUtils.h"

void ReadestAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  performAuthentication();
}

void ReadestAuthActivity::performAuthentication() {
  const auto result = ReadestSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == ReadestSyncClient::Error::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = ReadestSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void ReadestAuthActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadestAuthActivity::onExit() {
  Activity::onExit();
  WifiUtils::wifiOff();
}

void ReadestAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READEST_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_READEST_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ReadestAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}
