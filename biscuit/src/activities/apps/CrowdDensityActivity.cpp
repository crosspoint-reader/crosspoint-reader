#include "CrowdDensityActivity.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// Static instance
// ---------------------------------------------------------------------------

CrowdDensityActivity* CrowdDensityActivity::activeInstance = nullptr;

void CrowdDensityActivity::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeInstance || !buf) return;
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = pkt->payload;
  const uint16_t sig_len = pkt->rx_ctrl.sig_len;

  if (sig_len < 16) return;

  // Probe request subtype = 0x40 (FC byte 0 = 0x40)
  if ((payload[0] & 0xFC) != 0x40) return;

  // Source MAC at offset 10
  activeInstance->addMac(payload + 10);
}

void CrowdDensityActivity::addMac(const uint8_t* mac) {
  portENTER_CRITICAL(&dataMux);
  for (int i = 0; i < seenMacCount; i++) {
    if (memcmp(seenMacs[i], mac, 6) == 0) {
      portEXIT_CRITICAL(&dataMux);
      return;
    }
  }
  if (seenMacCount < MAX_MACS) {
    memcpy(seenMacs[seenMacCount], mac, 6);
    seenMacCount++;
  }
  portEXIT_CRITICAL(&dataMux);
}

// ---------------------------------------------------------------------------
// onEnter / onExit
// ---------------------------------------------------------------------------

void CrowdDensityActivity::onEnter() {
  Activity::onEnter();
  state = READY;
  memset(history, 0, sizeof(history));
  historyHead = 0;
  historyCount = 0;
  currentCount = 0;
  memset(seenMacs, 0, sizeof(seenMacs));
  seenMacCount = 0;
  promiscuousActive = false;
  requestUpdate();
}

void CrowdDensityActivity::startCapture() {
  lastSample = millis();
  lastDisplay = millis();
  windowStart = millis();

  RADIO.ensureWifi();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  activeInstance = this;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);

  promiscuousActive = true;
  state = CAPTURING;
  requestUpdate();
}

void CrowdDensityActivity::onExit() {
  Activity::onExit();
  if (promiscuousActive) {
    esp_wifi_set_promiscuous(false);
    promiscuousActive = false;
  }
  activeInstance = nullptr;
  RADIO.shutdown();
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void CrowdDensityActivity::loop() {
  if (state == READY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startCapture();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  const unsigned long now = millis();

  // Sample into history ring buffer every SAMPLE_INTERVAL_MS
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    portENTER_CRITICAL(&dataMux);
    currentCount = seenMacCount;
    portEXIT_CRITICAL(&dataMux);

    history[historyHead] = currentCount;
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;

    // Reset window
    portENTER_CRITICAL(&dataMux);
    memset(seenMacs, 0, sizeof(seenMacs));
    seenMacCount = 0;
    portEXIT_CRITICAL(&dataMux);

    windowStart = now;
    lastSample = now;
    requestUpdate();
  }

  // Periodic display refresh (shows live count between samples)
  if (now - lastDisplay >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void CrowdDensityActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == READY) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Crowd Density");
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 30, "Estimate nearby people via");
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "WiFi probe request counting.");
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + 40, "Press Confirm to start capture.");
    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Header (CAPTURING state)
  portENTER_CRITICAL(&dataMux);
  const int liveCount = seenMacCount;
  portEXIT_CRITICAL(&dataMux);

  char subtitle[24];
  snprintf(subtitle, sizeof(subtitle), "%d seen (30s window)", liveCount);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Crowd Density", subtitle);

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int leftPad = metrics.contentSidePadding;

  // Large count display
  char countBuf[8];
  snprintf(countBuf, sizeof(countBuf), "%d", liveCount);
  int countY = headerBottom + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, countY, countBuf, true, EpdFontFamily::BOLD);
  countY += renderer.getTextHeight(UI_12_FONT_ID) + 6;
  renderer.drawCenteredText(UI_10_FONT_ID, countY, "unique probe MACs");
  countY += renderer.getTextHeight(UI_10_FONT_ID) + 10;

  // "sampling..." indicator with elapsed time in current window
  {
    unsigned long elapsed = (millis() - windowStart) / 1000UL;
    char sampBuf[32];
    snprintf(sampBuf, sizeof(sampBuf), "sampling... %lus / 30s", elapsed);
    renderer.drawCenteredText(SMALL_FONT_ID, countY, sampBuf);
    countY += renderer.getTextHeight(SMALL_FONT_ID) + 14;
  }

  // Bar chart of history
  if (historyCount > 0) {
    const int chartTop = countY;
    const int chartBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 30;
    const int chartHeight = chartBottom - chartTop;

    if (chartHeight > 20) {
      const int chartWidth = pageWidth - leftPad * 2;
      const int barW = (historyCount > 0) ? (chartWidth / historyCount) : chartWidth;

      // Find max for scaling
      int maxVal = 1;
      for (int i = 0; i < historyCount; i++) {
        int slot = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
        if (history[slot] > maxVal) maxVal = history[slot];
      }

      renderer.drawLine(leftPad, chartTop, leftPad, chartBottom, true);
      renderer.drawLine(leftPad, chartBottom, leftPad + chartWidth, chartBottom, true);

      for (int i = 0; i < historyCount; i++) {
        int slot = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
        int val = history[slot];
        int bh = (val * (chartHeight - 2)) / maxVal;
        if (bh < 1 && val > 0) bh = 1;
        int bx = leftPad + i * barW;
        if (bh > 0) {
          renderer.fillRect(bx + 1, chartBottom - bh, barW > 2 ? barW - 1 : 1, bh, true);
        }
      }

      // Y-axis label (max value)
      char maxBuf[8];
      snprintf(maxBuf, sizeof(maxBuf), "%d", maxVal);
      renderer.drawText(SMALL_FONT_ID, 0, chartTop, maxBuf);
    }
  }

  // Disclaimer
  renderer.drawCenteredText(SMALL_FONT_ID,
                            pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 16,
                            "Estimate only — MAC randomization reduces accuracy");

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
