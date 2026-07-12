#include "SsidChannelActivity.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// Base64 — minimal implementation using standard alphabet.
// Kept in flash via static const table.
// ---------------------------------------------------------------------------

static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string SsidChannelActivity::base64Encode(const std::string& input) {
  std::string out;
  const size_t len = input.size();
  out.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    const uint8_t b0 = static_cast<uint8_t>(input[i]);
    const uint8_t b1 = (i + 1 < len) ? static_cast<uint8_t>(input[i + 1]) : 0;
    const uint8_t b2 = (i + 2 < len) ? static_cast<uint8_t>(input[i + 2]) : 0;

    out += kB64Chars[b0 >> 2];
    out += kB64Chars[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += (i + 1 < len) ? kB64Chars[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
    out += (i + 2 < len) ? kB64Chars[b2 & 0x3F] : '=';
  }
  return out;
}

std::string SsidChannelActivity::base64Decode(const std::string& input) {
  // Build decode table on stack — 128 bytes, fine for stack
  int8_t dec[128];
  for (int i = 0; i < 128; i++) dec[i] = -1;
  for (int i = 0; i < 64; i++) dec[static_cast<uint8_t>(kB64Chars[i])] = static_cast<int8_t>(i);

  std::string out;
  const size_t len = input.size();
  out.reserve((len / 4) * 3);

  for (size_t i = 0; i + 3 < len; i += 4) {
    const uint8_t c0 = static_cast<uint8_t>(input[i]);
    const uint8_t c1 = static_cast<uint8_t>(input[i + 1]);
    const uint8_t c2 = static_cast<uint8_t>(input[i + 2]);
    const uint8_t c3 = static_cast<uint8_t>(input[i + 3]);

    // Reject any character outside the base64 alphabet (except '=')
    if (c0 >= 128 || dec[c0] < 0) return {};
    if (c1 >= 128 || dec[c1] < 0) return {};

    const int8_t v0 = dec[c0];
    const int8_t v1 = dec[c1];
    out += static_cast<char>((v0 << 2) | (v1 >> 4));

    if (c2 != '=') {
      if (c2 >= 128 || dec[c2] < 0) return {};
      const int8_t v2 = dec[c2];
      out += static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2));

      if (c3 != '=') {
        if (c3 >= 128 || dec[c3] < 0) return {};
        const int8_t v3 = dec[c3];
        out += static_cast<char>(((v2 & 0x03) << 6) | v3);
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SsidChannelActivity::onEnter() {
  Activity::onEnter();
  state = MODE_SELECT;
  modeIndex = 0;
  outMessage.clear();
  broadcastSsid.clear();
  received.clear();
  msgIndex = 0;
  scanning = false;
  lastScan = 0;
  requestUpdate();
}

void SsidChannelActivity::onExit() {
  if (state == SENDING) {
    stopSending();
  } else if (state == RECEIVING || state == MESSAGE_LIST) {
    WiFi.scanDelete();
    RADIO.shutdown();
  }
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Sending helpers
// ---------------------------------------------------------------------------

void SsidChannelActivity::startSending() {
  RADIO.ensureWifi();
  std::string encoded = std::string(SSID_PREFIX) + base64Encode(outMessage);
  if (encoded.length() > 32) encoded.resize(32);
  broadcastSsid = encoded;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(encoded.c_str());
  sendStart = millis();
  state = SENDING;
  requestUpdate();
}

void SsidChannelActivity::stopSending() {
  WiFi.softAPdisconnect(true);
  RADIO.shutdown();
  broadcastSsid.clear();
}

// ---------------------------------------------------------------------------
// Receiving helpers
// ---------------------------------------------------------------------------

void SsidChannelActivity::startReceiving() {
  RADIO.ensureWifi();
  WiFi.mode(WIFI_STA);
  received.clear();
  msgIndex = 0;
  scanning = false;
  lastScan = 0;
  state = RECEIVING;
  requestUpdate();
}

void SsidChannelActivity::doScan() {
  scanning = true;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  const size_t prefixLen = strlen(SSID_PREFIX);
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.startsWith(SSID_PREFIX)) {
      std::string payload = ssid.c_str() + prefixLen;
      std::string decoded = base64Decode(payload);
      if (!decoded.empty()) {
        bool found = false;
        for (const auto& m : received) {
          if (m.text == decoded) { found = true; break; }
        }
        if (!found && static_cast<int>(received.size()) < MAX_RECEIVED) {
          received.push_back({decoded, static_cast<int8_t>(WiFi.RSSI(i)), millis()});
        }
      }
    }
  }
  WiFi.scanDelete();
  scanning = false;
  lastScan = millis();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void SsidChannelActivity::loop() {
  // ---- MODE_SELECT ----
  if (state == MODE_SELECT) {
    buttonNavigator.onNext([this] {
      modeIndex = ButtonNavigator::nextIndex(modeIndex, 2);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      modeIndex = ButtonNavigator::previousIndex(modeIndex, 2);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (modeIndex == 0) {
        // Send — launch keyboard to compose message
        state = SEND_COMPOSE;
        // Max 21 chars: base64(21) = 28 chars + "BC_" prefix (3) = 31 <= 32
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Enter Message", "", 21),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                outMessage = std::get<KeyboardResult>(result.data).text;
                if (!outMessage.empty()) {
                  startSending();
                } else {
                  state = MODE_SELECT;
                  requestUpdate();
                }
              } else {
                state = MODE_SELECT;
                requestUpdate();
              }
            });
      } else {
        startReceiving();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    return;
  }

  // SEND_COMPOSE — keyboard sub-activity is running; loop is idle
  if (state == SEND_COMPOSE) {
    return;
  }

  // ---- SENDING ----
  if (state == SENDING) {
    unsigned long elapsed = millis() - sendStart;
    if (elapsed >= SEND_DURATION_MS) {
      stopSending();
      state = MODE_SELECT;
      requestUpdate();
      return;
    }
    // Refresh countdown every second
    static unsigned long lastSendRefresh = 0;
    if (millis() - lastSendRefresh >= 1000) {
      lastSendRefresh = millis();
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      stopSending();
      state = MODE_SELECT;
      requestUpdate();
    }
    return;
  }

  // ---- RECEIVING ----
  if (state == RECEIVING) {
    // Auto-scan every SCAN_INTERVAL_MS
    unsigned long now = millis();
    if (!scanning && (lastScan == 0 || now - lastScan >= SCAN_INTERVAL_MS)) {
      doScan();
    }

    buttonNavigator.onNext([this] {
      if (!received.empty()) {
        msgIndex = ButtonNavigator::nextIndex(msgIndex, static_cast<int>(received.size()));
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this] {
      if (!received.empty()) {
        msgIndex = ButtonNavigator::previousIndex(msgIndex, static_cast<int>(received.size()));
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      RADIO.shutdown();
      state = MODE_SELECT;
      requestUpdate();
      return;
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SsidChannelActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case MODE_SELECT:   renderModeSelect();  break;
    case SEND_COMPOSE:  renderModeSelect();  break;  // keyboard overlaid by sub-activity
    case SENDING:       renderSending();     break;
    case RECEIVING:     renderReceiving();   break;
    case MESSAGE_LIST:  renderReceiving();   break;
  }

  renderer.displayBuffer();
}

void SsidChannelActivity::renderModeSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer,
      Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
      "SSID Channel");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  static const char* const kModeLabels[2] = {"Send Message", "Receive Messages"};
  static const char* const kModeSubtitles[2] = {
      "Broadcast encoded SSID",
      "Scan for encoded SSIDs"
  };

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, 2, modeIndex,
      [](int i) -> std::string { return kModeLabels[i]; },
      [](int i) -> std::string { return kModeSubtitles[i]; });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SsidChannelActivity::renderSending() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer,
      Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
      "SSID Channel", "BROADCASTING");

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 8;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Broadcasting message as SSID:");
  y += lineH + 4;

  // Show SSID being broadcast (truncated for display if very long)
  renderer.drawCenteredText(UI_12_FONT_ID, y, broadcastSsid.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 16;

  // Show decoded message for confirmation
  renderer.drawText(UI_10_FONT_ID, leftPad, y, "Message:");
  y += lineH;
  renderer.drawCenteredText(UI_10_FONT_ID, y, outMessage.c_str());
  y += lineH + 8;

  // Countdown
  unsigned long elapsed = millis() - sendStart;
  unsigned long remaining = (elapsed < SEND_DURATION_MS) ? (SEND_DURATION_MS - elapsed) / 1000 : 0;
  char countBuf[24];
  snprintf(countBuf, sizeof(countBuf), "Stops in: %lus", remaining);
  renderer.drawCenteredText(SMALL_FONT_ID, y, countBuf);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SsidChannelActivity::renderReceiving() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Build subtitle: scanning status + count
  char subtitle[32];
  if (scanning) {
    snprintf(subtitle, sizeof(subtitle), "Scanning...");
  } else {
    unsigned long elapsed = (lastScan > 0) ? (millis() - lastScan) / 1000 : 0;
    snprintf(subtitle, sizeof(subtitle), "%d found | next: %lus",
             static_cast<int>(received.size()),
             (elapsed < SCAN_INTERVAL_MS / 1000) ? (SCAN_INTERVAL_MS / 1000 - elapsed) : 0UL);
  }

  GUI.drawHeader(renderer,
      Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
      "SSID Channel", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (received.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
        scanning ? "Scanning for BC_ SSIDs..." : "No messages found yet");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(received.size()), msgIndex,
        [this](int i) -> std::string { return received[i].text; },
        [this](int i) -> std::string {
          char buf[24];
          snprintf(buf, sizeof(buf), "%ddBm", static_cast<int>(received[i].rssi));
          return buf;
        });
  }

  const auto labels = mappedInput.mapLabels("Back", "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
