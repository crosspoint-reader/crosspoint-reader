#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

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
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Turn on WiFi
  WiFi.mode(WIFI_STA);

  if (mode == Mode::PROMPT) {
    // Start at idle so the user can choose to login or register
    state = IDLE;
    requestUpdate();
  } else {
    // Skip the idle prompt and go straight to the requested action
    startWifi();
  }
}

void KOReaderAuthActivity::startWifi() {
  // If already connected, jump straight to the action
  if (WiFi.status() == WL_CONNECTED) {
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

    xTaskCreate(
        [](void* param) {
          auto* self = static_cast<KOReaderAuthActivity*>(param);
          if (self->mode == Mode::REGISTER) {
            self->performRegistration();
          } else {
            self->performAuthentication();
          }
          vTaskDelete(nullptr);
        },
        "AuthTask", 4096, this, 1, nullptr);
    return;
  }

  // Otherwise launch WiFi selection first
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void KOReaderAuthActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void KOReaderAuthActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == IDLE) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_AUTHENTICATE), tr(STR_REGISTER), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == AUTHENTICATING || state == REGISTERING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SUCCESS) {
    const char* successMsg = (mode == Mode::REGISTER) ? tr(STR_REGISTER_SUCCESS) : tr(STR_AUTH_SUCCESS);
    renderer.drawCenteredText(UI_10_FONT_ID, top, successMsg, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == IDLE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      mode = Mode::LOGIN;
      startWifi();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      mode = Mode::REGISTER;
      startWifi();
      return;
    }
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onComplete();
    }
  }
}
