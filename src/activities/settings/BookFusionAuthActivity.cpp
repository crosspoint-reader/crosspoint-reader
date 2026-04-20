#include "BookFusionAuthActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr int MAX_NETWORK_RETRIES = 3;
// Only re-render the countdown every 10 s to limit E-ink refreshes.
constexpr unsigned long TIMER_REFRESH_INTERVAL_MS = 10000;
constexpr int QR_CODE_SIZE = 198;
// User-facing activation URL. The OAuth response may return a different
// verification_uri; always display the short, memorable URL instead.
constexpr const char* DEVICE_ACTIVATION_URL = "https://bookfusion.com/device";
}  // namespace

void BookFusionAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = REQUESTING_CODE;
  }
  requestUpdate(true);

  requestCode();
}

void BookFusionAuthActivity::requestCode() {
  BookFusionDeviceCodeResponse resp;
  const auto result = BookFusionSyncClient::requestDeviceCode(resp);

  if (result != BookFusionSyncClient::OK) {
    LOG_ERR("BFAuth", "requestDeviceCode failed: %s", BookFusionSyncClient::errorString(result));
    RenderLock lock(*this);
    state = FAILED;
    requestUpdate(true);
    return;
  }

  strlcpy(deviceCode, resp.deviceCode, sizeof(deviceCode));
  strlcpy(userCode, resp.userCode, sizeof(userCode));
  strlcpy(verificationUri, resp.verificationUri, sizeof(verificationUri));
  pollIntervalSec = resp.interval;

  const unsigned long now = millis();
  pollExpireAt = now + static_cast<unsigned long>(resp.expiresIn) * 1000UL;
  nextPollAt = now + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
  lastTimerRefresh = now;
  networkRetries = 0;

  {
    RenderLock lock(*this);
    state = WAITING_FOR_USER;
  }
  requestUpdate(true);
}

void BookFusionAuthActivity::doPoll() {
  {
    RenderLock lock(*this);
    state = POLLING;
  }

  char tokenBuf[512] = {};
  const auto result = BookFusionSyncClient::pollForToken(deviceCode, tokenBuf, sizeof(tokenBuf));

  switch (result) {
    case BookFusionSyncClient::OK:
      BF_TOKEN_STORE.setToken(tokenBuf);
      BF_TOKEN_STORE.saveToFile();
      {
        RenderLock lock(*this);
        state = SUCCESS;
      }
      requestUpdate(true);
      return;

    case BookFusionSyncClient::PENDING:
      nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
      {
        RenderLock lock(*this);
        state = WAITING_FOR_USER;
      }
      return;

    case BookFusionSyncClient::SLOW_DOWN:
      pollIntervalSec += 5;
      nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
      {
        RenderLock lock(*this);
        state = WAITING_FOR_USER;
      }
      return;

    case BookFusionSyncClient::EXPIRED: {
      RenderLock lock(*this);
      state = EXPIRED;
    }
      requestUpdate(true);
      return;

    case BookFusionSyncClient::DENIED: {
      RenderLock lock(*this);
      state = DENIED;
    }
      requestUpdate(true);
      return;

    case BookFusionSyncClient::NETWORK_ERROR:
      networkRetries++;
      if (networkRetries > MAX_NETWORK_RETRIES) {
        RenderLock lock(*this);
        state = FAILED;
        requestUpdate(true);
      } else {
        nextPollAt = millis() + static_cast<unsigned long>(pollIntervalSec) * 1000UL;
        RenderLock lock(*this);
        state = WAITING_FOR_USER;
      }
      return;

    default: {
      RenderLock lock(*this);
      state = FAILED;
    }
      requestUpdate(true);
      return;
  }
}

void BookFusionAuthActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionAuthActivity::onExit() {
  Activity::onExit();
  // Leave WiFi on — BookFusionSettingsActivity called from settings which may need it.
  // The caller (SettingsActivity) doesn't do network ops, so we let the main flow handle WiFi.
}

void BookFusionAuthActivity::loop() {
  if (state == SUCCESS || state == EXPIRED || state == DENIED || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (state == WAITING_FOR_USER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const unsigned long now = millis();

    // Check expiry
    if (now - pollExpireAt < 0x80000000UL && now >= pollExpireAt) {
      {
        RenderLock lock(*this);
        state = EXPIRED;
      }
      requestUpdate(true);
      return;
    }

    // Refresh countdown display every TIMER_REFRESH_INTERVAL_MS
    if (now - lastTimerRefresh >= TIMER_REFRESH_INTERVAL_MS) {
      lastTimerRefresh = now;
      requestUpdate();
    }

    // Time to poll?
    if (now >= nextPollAt) {
      doPoll();
    }
  }
}

void BookFusionAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BF_AUTH));

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (state == REQUESTING_CODE || state == CONNECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, tr(STR_BF_WAITING));
    renderer.displayBuffer();
    return;
  }

  if (state == WAITING_FOR_USER || state == POLLING) {
    int y = contentTop;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_VISIT_URL), true, EpdFontFamily::BOLD);
    y += lineH + 4;

    renderer.drawCenteredText(UI_12_FONT_ID, y, DEVICE_ACTIVATION_URL, true, EpdFontFamily::REGULAR);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_OR_SCAN_QR), true, EpdFontFamily::REGULAR);
    y += lineH + 8;

    const Rect qrBounds((pageWidth - QR_CODE_SIZE) / 2, y, QR_CODE_SIZE, QR_CODE_SIZE);
    QrUtils::drawQrCode(renderer, qrBounds, DEVICE_ACTIVATION_URL);
    y += QR_CODE_SIZE + 12;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_BF_ENTER_CODE), true, EpdFontFamily::BOLD);
    y += lineH + 8;

    renderer.drawCenteredText(UI_12_FONT_ID, y, userCode, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 12;

    const unsigned long now = millis();
    const int secsLeft = (pollExpireAt > now) ? static_cast<int>((pollExpireAt - now) / 1000UL) : 0;
    char countdown[32];
    snprintf(countdown, sizeof(countdown), tr(STR_BF_TIME_REMAINING), secsLeft);
    renderer.drawCenteredText(UI_10_FONT_ID, y, countdown);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, tr(STR_BF_LINK_SUCCESS), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Error states: EXPIRED, DENIED, FAILED
  const char* msg = tr(STR_BF_AUTH_FAILED);
  if (state == EXPIRED) msg = tr(STR_BF_CODE_EXPIRED);
  if (state == DENIED) msg = tr(STR_BF_AUTH_DENIED);
  renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, msg, true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
