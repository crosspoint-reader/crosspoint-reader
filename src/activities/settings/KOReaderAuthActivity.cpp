#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
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
    if (mode == Mode::REGISTER) {
      state = REGISTERING;
      statusMessage = tr(STR_REGISTERING);
    } else {
      state = AUTHENTICATING;
      statusMessage = tr(STR_AUTHENTICATING);
    }
  }
  requestUpdate();

  if (mode == Mode::REGISTER) {
    performRegistration();
  } else {
    performAuthentication();
  }
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::performRegistration() {
  const auto result = KOReaderSyncClient::registerUser();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_REGISTER_SUCCESS);
    } else if (result == KOReaderSyncClient::USER_EXISTS) {
      state = USER_EXISTS;
      errorMessage = KOReaderSyncClient::errorString(result);
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();

  HalClock::wifiOff();
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = contentRect.y + (contentRect.height - height) / 2;

  if (state == AUTHENTICATING || state == REGISTERING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    const char* successMsg = (mode == Mode::REGISTER) ? tr(STR_REGISTER_SUCCESS) : tr(STR_AUTH_SUCCESS);
    renderer.drawCenteredText(UI_10_FONT_ID, top, successMsg, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == USER_EXISTS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_USERNAME_TAKEN), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  } else if (state == FAILED) {
    const char* failedMsg = (mode == Mode::REGISTER) ? tr(STR_REGISTER_FAILED) : tr(STR_AUTH_FAILED);
    renderer.drawCenteredText(UI_10_FONT_ID, top, failedMsg, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED || state == USER_EXISTS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}
