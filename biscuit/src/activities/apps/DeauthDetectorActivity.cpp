#include "DeauthDetectorActivity.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// Static instance pointer for the C promiscuous callback
static DeauthDetectorActivity* activeDetector = nullptr;

static void IRAM_ATTR deauthCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeDetector || !buf || type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  activeDetector->onPacket(pkt->payload, pkt->rx_ctrl.sig_len, pkt->rx_ctrl.rssi);
}

// ---------------------------------------------------------------------------
// onPacket — called from ISR context, must be fast, no heap
// ---------------------------------------------------------------------------
void DeauthDetectorActivity::onPacket(const uint8_t* data, uint16_t len, int rssi) {
  if (len < 24) return;  // minimum 802.11 management header

  uint8_t frameType = (data[0] >> 2) & 0x03;
  uint8_t subType   = (data[0] >> 4) & 0x0F;

  if (frameType != 0) return;  // not a management frame

  portENTER_CRITICAL_ISR(&statsMux);
  totalFrames++;

  bool isDeauth   = (subType == 0x0C);  // Deauthentication
  bool isDisassoc = (subType == 0x0A);  // Disassociation

  if (isDeauth || isDisassoc) {
    if (isDeauth) {
      deauthCount++;
      intervalDeauth++;
    } else {
      disassocCount++;
    }

    // Log into ring buffer (no heap, fixed array)
    int idx = eventLogHead;
    memcpy(eventLog[idx].srcMac, data + 10, 6);  // Address 2 = transmitter
    memcpy(eventLog[idx].dstMac, data + 4,  6);  // Address 1 = receiver
    eventLog[idx].type = isDeauth ? 0 : 1;
    eventLog[idx].rssi = static_cast<int8_t>(rssi);
    eventLogHead = (eventLogHead + 1) % EVENT_LOG_SIZE;
    if (eventLogCount < EVENT_LOG_SIZE) eventLogCount++;
  }
  portEXIT_CRITICAL_ISR(&statsMux);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void DeauthDetectorActivity::onEnter() {
  Activity::onEnter();
  state           = IDLE;
  deauthCount     = 0;
  disassocCount   = 0;
  totalFrames     = 0;
  intervalDeauth  = 0;
  eventLogHead    = 0;
  eventLogCount   = 0;
  spikeCount      = 0;
  currentChannel  = 1;
  autoHop         = true;
  selectorIndex   = 0;
  lastUpdateTime  = millis();
  lastHopTime     = millis();
  memset(eventLog, 0, sizeof(eventLog));
  requestUpdate();
}

void DeauthDetectorActivity::onExit() {
  Activity::onExit();
  if (state != IDLE) stopMonitor();
}

DeauthDetectorActivity::~DeauthDetectorActivity() {
  if (activeDetector == this) {
    esp_wifi_set_promiscuous(false);
    activeDetector = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Start / stop promiscuous monitoring
// ---------------------------------------------------------------------------
void DeauthDetectorActivity::startMonitor() {
  RADIO.ensureWifi();
  WiFi.disconnect();
  activeDetector = this;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(deauthCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHopTime    = millis();
  lastUpdateTime = millis();
  state          = MONITORING;
}

void DeauthDetectorActivity::stopMonitor() {
  esp_wifi_set_promiscuous(false);
  activeDetector = nullptr;
  RADIO.shutdown();
  state = IDLE;
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void DeauthDetectorActivity::loop() {
  if (state == IDLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startMonitor();
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == ALERT_SPIKE) {
    // Any button dismisses the alert and returns to monitoring
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      state = MONITORING;
      requestUpdate();
    }
    return;
  }

  // state == MONITORING
  unsigned long now = millis();

  // Channel hopping
  if (autoHop && (now - lastHopTime >= HOP_INTERVAL_MS)) {
    currentChannel = static_cast<uint8_t>((currentChannel % 13) + 1);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastHopTime = now;
  }

  // Periodic stats snapshot + spike check
  if (now - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    lastUpdateTime = now;

    portENTER_CRITICAL(&statsMux);
    uint32_t recent  = intervalDeauth;
    intervalDeauth   = 0;
    int currentCount = eventLogCount;
    portEXIT_CRITICAL(&statsMux);

    // Clamp scroll position now that we hold a consistent count
    if (currentCount > 0 && selectorIndex >= currentCount) {
      selectorIndex = currentCount - 1;
    }

    if (recent >= static_cast<uint32_t>(SPIKE_THRESHOLD)) {
      spikeCount = recent;
      state      = ALERT_SPIKE;
    }
    requestUpdate();
  }

  // Scroll event log
  buttonNavigator.onNext([this] {
    portENTER_CRITICAL(&statsMux);
    int count = eventLogCount;
    portEXIT_CRITICAL(&statsMux);
    if (count > 0) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    portENTER_CRITICAL(&statsMux);
    int count = eventLogCount;
    portEXIT_CRITICAL(&statsMux);
    if (count > 0) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
      requestUpdate();
    }
  });

  // Toggle auto-hop on Confirm
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    autoHop = !autoHop;
    requestUpdate();
  }

  // Stop monitoring and exit on Back
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    stopMonitor();
    finish();
  }
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------
static void formatMac(char* buf, int bufLen, const uint8_t* mac) {
  snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void DeauthDetectorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // ---- IDLE ----------------------------------------------------------------
  if (state == IDLE) {
    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Deauth Detector");

    const int midY = (metrics.topPadding + metrics.headerHeight + pageHeight - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 30,
                              "WiFi deauth/disassoc frame detector.");
    renderer.drawCenteredText(UI_10_FONT_ID, midY,
                              "Press OK to start monitoring.");

    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ---- ALERT_SPIKE ---------------------------------------------------------
  if (state == ALERT_SPIKE) {
    GUI.drawHeader(renderer,
                   Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Deauth Detector");

    const int midY = (metrics.topPadding + metrics.headerHeight + pageHeight - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 40, "SPIKE DETECTED", true, EpdFontFamily::BOLD);

    char buf[48];
    snprintf(buf, sizeof(buf), "%lu deauths in last interval", static_cast<unsigned long>(spikeCount));
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, buf);
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 50, "Press any button to continue");

    GUI.drawButtonHints(renderer, "Dismiss", "", "", "");
    renderer.displayBuffer();
    return;
  }

  // ---- MONITORING ----------------------------------------------------------
  // Take a consistent snapshot of volatile counters AND the event log.
  // Both reads happen inside the same critical section to prevent torn reads
  // from the promiscuous ISR callback writing concurrently.
  portENTER_CRITICAL(&statsMux);
  uint32_t snapTotal    = totalFrames;
  uint32_t snapDeauth   = deauthCount;
  uint32_t snapDisassoc = disassocCount;
  int      snapCount    = eventLogCount;
  DeauthEvent displayEvents[EVENT_LOG_SIZE];
  int         displayCount = min(snapCount, (int)EVENT_LOG_SIZE);
  int         displayHead  = eventLogHead;
  memcpy(displayEvents, eventLog, sizeof(displayEvents));
  portEXIT_CRITICAL(&statsMux);

  // Header with channel info
  char chBuf[16];
  snprintf(chBuf, sizeof(chBuf), "Ch:%u%s", currentChannel, autoHop ? " auto" : "");
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Deauth Detector", chBuf);

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 6;
  const int fontH = renderer.getTextHeight(UI_10_FONT_ID);

  // Stats row
  char statBuf[80];
  snprintf(statBuf, sizeof(statBuf), "Frames: %lu   Deauth: %lu   Disassoc: %lu",
           static_cast<unsigned long>(snapTotal),
           static_cast<unsigned long>(snapDeauth),
           static_cast<unsigned long>(snapDisassoc));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, statBuf, true, EpdFontFamily::BOLD);
  y += fontH + 8;

  // Divider
  renderer.drawLine(leftPad, y, pageWidth - leftPad, y, true);
  y += 6;

  if (displayCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + 20, "No deauth/disassoc frames captured yet.");
  } else {
    // The ring buffer is stored newest-first relative to head:
    // index 0 in our logical list = most recent event
    // logical index i maps to array index: (head - 1 - i + SIZE) % SIZE
    const int listAreaTop = y;
    const int listAreaH   = pageHeight - metrics.buttonHintsHeight - listAreaTop - 4;

    GUI.drawList(
        renderer,
        Rect{0, listAreaTop, pageWidth, listAreaH},
        displayCount,
        selectorIndex,
        [&](int i) -> std::string {
          int arrIdx = (displayHead - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
          const DeauthEvent& ev = displayEvents[arrIdx];
          char label[32];
          char mac[18];
          formatMac(mac, sizeof(mac), ev.srcMac);
          snprintf(label, sizeof(label), "%s  %s",
                   ev.type == 0 ? "DEAUTH" : "DISASSOC", mac);
          return label;
        },
        [&](int i) -> std::string {
          int arrIdx = (displayHead - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE;
          const DeauthEvent& ev = displayEvents[arrIdx];
          char sub[40];
          char dst[18];
          formatMac(dst, sizeof(dst), ev.dstMac);
          snprintf(sub, sizeof(sub), "-> %s  RSSI:%d", dst, static_cast<int>(ev.rssi));
          return sub;
        });
  }

  const auto labels = mappedInput.mapLabels("Stop", autoHop ? "Manual" : "Auto", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Scroll", "Scroll");

  renderer.displayBuffer();
}
