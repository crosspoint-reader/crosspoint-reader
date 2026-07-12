#include "ApClonerActivity.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "util/RadioManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ApClonerActivity::onEnter() {
  Activity::onEnter();
  state = SCANNING;
  aps.clear();
  selectorIndex = 0;
  cloneActive = false;
  RADIO.ensureWifi();
  WiFi.disconnect();
  startScan();
  requestUpdate();
}

void ApClonerActivity::onExit() {
  stopClone();
  WiFi.scanDelete();
  RADIO.shutdown();
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Scan helpers
// ---------------------------------------------------------------------------

void ApClonerActivity::startScan() {
  state = SCANNING;
  aps.clear();
  selectorIndex = 0;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async
}

void ApClonerActivity::processScanResults() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  aps.clear();
  if (result > 0) {
    aps.reserve(static_cast<size_t>(result));
    for (int i = 0; i < result; i++) {
      AP ap;
      ap.ssid = WiFi.SSID(i).c_str();
      if (ap.ssid.empty()) ap.ssid = "(hidden)";
      ap.rssi = WiFi.RSSI(i);
      ap.channel = static_cast<uint8_t>(WiFi.channel(i));
      ap.encType = static_cast<uint8_t>(WiFi.encryptionType(i));
      aps.push_back(std::move(ap));
    }
    std::sort(aps.begin(), aps.end(), [](const AP& a, const AP& b) { return a.rssi > b.rssi; });
  }
  WiFi.scanDelete();
  state = SELECT_AP;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Clone helpers
// ---------------------------------------------------------------------------

void ApClonerActivity::startClone(int apIndex) {
  if (apIndex < 0 || apIndex >= static_cast<int>(aps.size())) return;

  WiFi.scanDelete();
  clonedSsid = aps[apIndex].ssid;
  clonedChannel = aps[apIndex].channel;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(clonedSsid.c_str(), nullptr, clonedChannel, 0, 4);
  cloneActive = true;
  cloneStartTime = millis();
  connectedClients = 0;
  lastUpdateTime = 0;
  state = CLONE_RUNNING;
  requestUpdate();
}

void ApClonerActivity::stopClone() {
  if (cloneActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    cloneActive = false;
  }
}

// ---------------------------------------------------------------------------
// Encryption label
// ---------------------------------------------------------------------------

const char* ApClonerActivity::encryptionString(uint8_t type) {
  switch (type) {
    case WIFI_AUTH_OPEN:         return "Open";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:return "WPA2/3";
    default:                     return "?";
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void ApClonerActivity::loop() {
  if (state == SCANNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    processScanResults();
    return;
  }

  if (state == SELECT_AP) {
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
      if (!aps.empty()) {
        startClone(selectorIndex);
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      startScan();
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // CLONE_RUNNING state
  connectedClients = WiFi.softAPgetStationNum();
  unsigned long now = millis();
  if (now - lastUpdateTime >= 2000) {
    lastUpdateTime = now;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    stopClone();
    state = SELECT_AP;
    startScan();
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void ApClonerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // ---- SCANNING ----
  if (state == SCANNING) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "AP Cloner");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SCANNING_WIFI));
    renderer.displayBuffer();
    return;
  }

  // ---- SELECT_AP ----
  if (state == SELECT_AP) {
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "%d found", static_cast<int>(aps.size()));
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "AP Cloner", subtitle);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (aps.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(aps.size()), selectorIndex,
          [this](int index) -> std::string { return aps[index].ssid; },
          [this](int index) -> std::string {
            char buf[40];
            snprintf(buf, sizeof(buf), "Ch%d  %ddBm  %s",
                     static_cast<int>(aps[index].channel),
                     static_cast<int>(aps[index].rssi),
                     encryptionString(aps[index].encType));
            return buf;
          });
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Clone", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ---- CLONE_RUNNING ----
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "AP Cloner", "ACTIVE");

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 16;
  const int lineSpacing = 48;

  // Large SSID centered
  renderer.drawCenteredText(UI_12_FONT_ID, y, clonedSsid.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getTextHeight(UI_12_FONT_ID) + 16;

  // Channel
  char chanBuf[24];
  snprintf(chanBuf, sizeof(chanBuf), "Channel: %d", static_cast<int>(clonedChannel));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, chanBuf);
  y += lineSpacing;

  // Connected clients
  char clientBuf[32];
  snprintf(clientBuf, sizeof(clientBuf), "Connected: %d", connectedClients);
  renderer.drawText(UI_10_FONT_ID, leftPad, y, clientBuf);
  y += lineSpacing;

  // Uptime
  unsigned long elapsed = millis() - cloneStartTime;
  unsigned long totalSecs = elapsed / 1000UL;
  unsigned long mins = totalSecs / 60UL;
  unsigned long secs = totalSecs % 60UL;
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "Uptime: %lum %lus", mins, secs);
  renderer.drawText(UI_10_FONT_ID, leftPad, y, uptimeBuf);
  y += lineSpacing;

  // IP address
  String ip = WiFi.softAPIP().toString();
  char ipBuf[40];
  snprintf(ipBuf, sizeof(ipBuf), "IP: %s", ip.c_str());
  renderer.drawText(UI_10_FONT_ID, leftPad, y, ipBuf);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
