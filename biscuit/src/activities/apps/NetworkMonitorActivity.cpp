#include "NetworkMonitorActivity.h"

#include <HalStorage.h>
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

// ---------------------------------------------------------------------------
// Static C callback — must be a plain function, not a method
// ---------------------------------------------------------------------------

static NetworkMonitorActivity* activeNetworkMonitor = nullptr;

static void networkMonitorPromiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeNetworkMonitor || !buf) return;
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = pkt->payload;
  const uint16_t sig_len = pkt->rx_ctrl.sig_len;

  if (sig_len < 26) return;

  // Frame control byte 0: subtype in upper nibble
  // 0xC0 = management disassociation frame, 0xA0 = management disassociation variant
  const uint8_t frameCtrl = payload[0];
  if (frameCtrl != 0xC0 && frameCtrl != 0xA0) return;

  activeNetworkMonitor->onFrame(payload, sig_len, pkt->rx_ctrl.rssi,
                                static_cast<uint8_t>(pkt->rx_ctrl.channel));
}

// ---------------------------------------------------------------------------
// onFrame — called from the WiFi RX task, must be ISR-safe
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::onFrame(const uint8_t* payload, uint16_t len, int rssi,
                                     uint8_t channel) {
  portENTER_CRITICAL(&dataMux);

  totalFrames = totalFrames + 1;
  framesThisInterval = framesThisInterval + 1;

  // 802.11 management frame layout (fixed header):
  //   [0]   Frame Control byte 0  (type/subtype)
  //   [1]   Frame Control byte 1  (flags)
  //   [2-3] Duration
  //   [4-9] Destination MAC
  //   [10-15] Source MAC
  //   [16-21] BSSID (target)
  //   [22-23] Sequence control
  //   [24]  Reason code (body)
  const uint8_t* srcMac  = payload + 10;
  const uint8_t* bssid   = payload + 16;
  const uint8_t  reason  = (len > 24) ? payload[24] : 0;
  const uint8_t  ftype   = payload[0];

  // Search for an existing event matching source MAC + BSSID
  for (int i = 0; i < eventCount; i++) {
    if (memcmp(events[i].attackerMac, srcMac, 6) == 0 &&
        memcmp(events[i].targetBssid, bssid,  6) == 0) {
      events[i].count++;
      events[i].lastSeen = millis();
      events[i].rssi     = static_cast<int8_t>(rssi);
      portEXIT_CRITICAL(&dataMux);
      return;
    }
  }

  // New event
  if (eventCount < MAX_EVENTS) {
    DetectionEvent& ev = events[eventCount];
    memcpy(ev.attackerMac, srcMac, 6);
    memcpy(ev.targetBssid, bssid,  6);
    ev.channel    = channel;
    ev.rssi       = static_cast<int8_t>(rssi);
    ev.count      = 1;
    ev.firstSeen  = millis();
    ev.lastSeen   = millis();
    ev.frameType  = ftype;
    ev.reasonCode = reason;
    eventCount++;
  }

  portEXIT_CRITICAL(&dataMux);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::onEnter() {
  Activity::onEnter();

  monitorMode   = FRAME_DETECTION;
  state         = MONITORING;
  selectorIndex = 0;
  detailIndex   = 0;
  alertActive   = false;
  autoHop       = true;
  currentChannel = 1;
  historyIndex  = 0;
  framesPerSec  = 0;
  totalFrames   = 0;
  framesThisInterval = 0;
  eventCount    = 0;

  memset(events,      0, sizeof(events));
  memset(rateHistory, 0, sizeof(rateHistory));

  lastHopTime    = millis();
  lastUpdateTime = millis();

  // Reset rogue AP state
  allAps.clear();
  ssidGroups.clear();
  rogueScanPhase = 0;
  roguesScanCount = 0;
  suspiciousCount = 0;
  rogueDetailGroupIndex = -1;
  rogueSelectorIndex = 0;

  startMonitoring();
  requestUpdate();
}

void NetworkMonitorActivity::onExit() {
  Activity::onExit();
  if (monitorMode == FRAME_DETECTION) {
    stopMonitoring();
  } else {
    WiFi.scanDelete();
    RADIO.shutdown();
  }
}

void NetworkMonitorActivity::startMonitoring() {
  RADIO.ensureWifi();
  WiFi.disconnect();

  activeNetworkMonitor = this;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(networkMonitorPromiscuousCallback);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  LOG_DBG("NETMON", "Frame monitoring started on ch%d", currentChannel);
}

void NetworkMonitorActivity::stopMonitoring() {
  esp_wifi_set_promiscuous(false);
  activeNetworkMonitor = nullptr;
  RADIO.shutdown();
  LOG_DBG("NETMON", "Frame monitoring stopped, %d events", eventCount);
}

// ---------------------------------------------------------------------------
// Rogue AP scan methods
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::startRogueScan() {
  state = ROGUE_SCANNING;
  if (rogueScanPhase == 0) {
    allAps.clear();
    ssidGroups.clear();
  }
  roguesScanCount++;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async
  requestUpdate();
}

void NetworkMonitorActivity::processRogueScanResults() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  if (result > 0) {
    for (int i = 0; i < result; i++) {
      ApRecord rec;
      rec.ssid = WiFi.SSID(i).c_str();
      if (rec.ssid.empty()) rec.ssid = "(hidden)";
      rec.rssi = WiFi.RSSI(i);
      rec.channel = static_cast<uint8_t>(WiFi.channel(i));
      rec.encType = static_cast<uint8_t>(WiFi.encryptionType(i));

      uint8_t* bssid = WiFi.BSSID(i);
      char buf[20];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
      rec.bssid = buf;

      // Deduplicate by BSSID — update RSSI if we already have this AP
      bool found = false;
      for (auto& existing : allAps) {
        if (existing.bssid == rec.bssid) {
          existing.rssi = rec.rssi;
          found = true;
          break;
        }
      }
      if (!found && static_cast<int>(allAps.size()) < 200) {
        allAps.push_back(std::move(rec));
      }
    }
  }

  WiFi.scanDelete();
  rogueScanPhase++;

  if (rogueScanPhase < ROGUE_SCAN_PHASES) {
    startRogueScan();
  } else {
    analyzeGroups();
    state = ROGUE_RESULTS;
    rogueSelectorIndex = 0;
    requestUpdate();
  }
}

void NetworkMonitorActivity::analyzeGroups() {
  ssidGroups.clear();
  suspiciousCount = 0;

  // Group APs by SSID
  for (const auto& ap : allAps) {
    bool found = false;
    for (auto& g : ssidGroups) {
      if (g.ssid == ap.ssid) {
        g.apCount++;
        found = true;
        break;
      }
    }
    if (!found) {
      SsidGroup g;
      g.ssid = ap.ssid;
      g.apCount = 1;
      g.suspicious = false;
      g.mixedEncryption = false;
      g.mixedChannels = false;
      ssidGroups.push_back(std::move(g));
    }
  }

  // Analyse each group for anomalies
  for (auto& g : ssidGroups) {
    if (g.apCount <= 1) continue;

    // Multiple APs with the same SSID is suspicious
    g.suspicious = true;

    uint8_t firstEnc = 0xFF;
    uint8_t firstCh = 0xFF;
    bool encSet = false;
    bool chSet = false;

    for (const auto& ap : allAps) {
      if (ap.ssid != g.ssid) continue;

      if (!encSet) {
        firstEnc = ap.encType;
        encSet = true;
      } else if (ap.encType != firstEnc) {
        g.mixedEncryption = true;
      }

      if (!chSet) {
        firstCh = ap.channel;
        chSet = true;
      } else if (ap.channel != firstCh) {
        g.mixedChannels = true;
      }
    }
  }

  // Count suspicious groups
  for (const auto& g : ssidGroups) {
    if (g.suspicious) suspiciousCount++;
  }

  // Sort: suspicious first, then by apCount descending
  std::sort(ssidGroups.begin(), ssidGroups.end(), [](const SsidGroup& a, const SsidGroup& b) {
    if (a.suspicious != b.suspicious) return a.suspicious > b.suspicious;
    return a.apCount > b.apCount;
  });
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::loop() {
  // ---- DETAIL view (frame detection detail) ----
  if (state == DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MONITORING;
      requestUpdate();
    }
    return;
  }

  // ---- ROGUE_SCANNING state ----
  if (state == ROGUE_SCANNING) {
    processRogueScanResults();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Cancel rogue scan, return to frame detection mode
      WiFi.scanDelete();
      monitorMode = FRAME_DETECTION;
      state = MONITORING;
      startMonitoring();
      requestUpdate();
    }
    return;
  }

  // ---- ROGUE_DETAIL state ----
  if (state == ROGUE_DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = ROGUE_RESULTS;
      requestUpdate();
    }
    return;
  }

  // ---- ROGUE_RESULTS state ----
  if (state == ROGUE_RESULTS) {
    const int count = static_cast<int>(ssidGroups.size());

    buttonNavigator.onNext([this, count] {
      if (count > 0) {
        rogueSelectorIndex = ButtonNavigator::nextIndex(rogueSelectorIndex, count);
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this, count] {
      if (count > 0) {
        rogueSelectorIndex = ButtonNavigator::previousIndex(rogueSelectorIndex, count);
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (mappedInput.getHeldTime() >= 500) {
        // Long press: switch back to frame detection mode
        WiFi.scanDelete();
        allAps.clear();
        ssidGroups.clear();
        rogueScanPhase = 0;
        roguesScanCount = 0;
        suspiciousCount = 0;
        monitorMode = FRAME_DETECTION;
        state = MONITORING;
        startMonitoring();
        requestUpdate();
      } else if (!ssidGroups.empty()) {
        rogueDetailGroupIndex = rogueSelectorIndex;
        state = ROGUE_DETAIL;
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // ---- MONITORING state (frame detection) ----

  const unsigned long now = millis();

  // Channel hopping
  if (autoHop && (now - lastHopTime >= HOP_INTERVAL_MS)) {
    lastHopTime = now;
    currentChannel = static_cast<uint8_t>((currentChannel % 13) + 1);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  }

  // Periodic stats update
  if (now - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    const unsigned long elapsed = now - lastUpdateTime;

    portENTER_CRITICAL(&dataMux);
    const uint32_t frames = framesThisInterval;
    framesThisInterval = 0;
    portEXIT_CRITICAL(&dataMux);

    framesPerSec = (elapsed > 0) ? (frames * 1000UL) / elapsed : 0;

    rateHistory[historyIndex] = static_cast<uint16_t>(
        framesPerSec > 0xFFFF ? 0xFFFF : framesPerSec);
    historyIndex = (historyIndex + 1) % GRAPH_POINTS;

    alertActive    = (framesPerSec > 10);
    lastUpdateTime = now;
    requestUpdate();
  }

  // Navigation in event list
  portENTER_CRITICAL(&dataMux);
  const int count = eventCount;
  portEXIT_CRITICAL(&dataMux);

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

  // Confirm: open detail view (short press) or switch to AP scan mode (long press)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= 500) {
      // Long press: switch to rogue AP scan mode
      stopMonitoring();
      monitorMode = ROGUE_AP_SCAN;
      rogueScanPhase = 0;
      roguesScanCount = 0;
      suspiciousCount = 0;
      rogueDetailGroupIndex = -1;
      rogueSelectorIndex = 0;
      RADIO.ensureWifi();
      WiFi.disconnect();
      startRogueScan();
    } else if (count > 0) {
      detailIndex = selectorIndex;
      state = DETAIL;
    }
    requestUpdate();
    return;
  }

  // Manual channel control (disables auto-hop)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    autoHop = false;
    if (currentChannel > 1) {
      currentChannel--;
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    autoHop = false;
    if (currentChannel < 13) {
      currentChannel++;
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case ROGUE_SCANNING: renderRogueScanning(); break;
    case ROGUE_RESULTS:  renderRogueResults();  break;
    case ROGUE_DETAIL:   renderRogueDetail();   break;
    default:             renderFrameDetection(); break;
  }

  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// renderFrameDetection — MONITORING and DETAIL views
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::renderFrameDetection() {
  const auto& metrics  = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // ---- DETAIL view ----
  if (state == DETAIL) {
    portENTER_CRITICAL(&dataMux);
    const bool validIndex = (detailIndex >= 0 && detailIndex < eventCount);
    DetectionEvent ev = validIndex ? events[detailIndex] : DetectionEvent{};
    portEXIT_CRITICAL(&dataMux);

    if (!validIndex) {
      state = MONITORING;
    } else {
      GUI.drawHeader(renderer,
                     Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                     "Detection Detail");

      const int leftPad = metrics.contentSidePadding;
      int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
      constexpr int lineH = 45;

      // Source MAC
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Source MAC", true, EpdFontFamily::BOLD);
      y += 22;
      std::string srcStr = macToString(ev.attackerMac);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, srcStr.c_str());
      y += lineH;

      // Target BSSID
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Target BSSID", true, EpdFontFamily::BOLD);
      y += 22;
      std::string bssidStr = macToString(ev.targetBssid);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, bssidStr.c_str());
      y += lineH;

      // Channel
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Channel", true, EpdFontFamily::BOLD);
      y += 22;
      char chBuf[8];
      snprintf(chBuf, sizeof(chBuf), "%d", ev.channel);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, chBuf);
      y += lineH;

      // Type
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Frame Type", true, EpdFontFamily::BOLD);
      y += 22;
      renderer.drawText(UI_10_FONT_ID, leftPad, y,
                        (ev.frameType == 0xA0) ? "Disassociation" : "Management Frame");
      y += lineH;

      // Reason
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Reason", true, EpdFontFamily::BOLD);
      y += 22;
      char reasonBuf[48];
      snprintf(reasonBuf, sizeof(reasonBuf), "%d (%s)", ev.reasonCode,
               reasonCodeStr(ev.reasonCode));
      renderer.drawText(UI_10_FONT_ID, leftPad, y, reasonBuf);
      y += lineH;

      // Count
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Count", true, EpdFontFamily::BOLD);
      y += 22;
      char cntBuf[16];
      snprintf(cntBuf, sizeof(cntBuf), "%lu", static_cast<unsigned long>(ev.count));
      renderer.drawText(UI_10_FONT_ID, leftPad, y, cntBuf);
      y += lineH;

      // RSSI
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "RSSI", true, EpdFontFamily::BOLD);
      y += 22;
      char rssiBuf[16];
      snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", static_cast<int>(ev.rssi));
      renderer.drawText(UI_10_FONT_ID, leftPad, y, rssiBuf);
      y += lineH;

      // First / last seen
      renderer.drawText(SMALL_FONT_ID, leftPad, y, "First seen", true, EpdFontFamily::BOLD);
      y += 22;
      char timeBuf[24];
      unsigned long ageSec = (millis() - ev.firstSeen) / 1000UL;
      snprintf(timeBuf, sizeof(timeBuf), "%lus ago", ageSec);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, timeBuf);
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Last seen", true, EpdFontFamily::BOLD);
      y += 22;
      ageSec = (millis() - ev.lastSeen) / 1000UL;
      snprintf(timeBuf, sizeof(timeBuf), "%lus ago", ageSec);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, timeBuf);

      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    return;
  }

  // ---- MONITORING view ----

  // Header subtitle: channel + mode
  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "Ch %d (%s)", currentChannel,
           autoHop ? "auto" : "manual");
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Network Monitor", subtitle);

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 6;

  // Alert banner
  if (alertActive) {
    renderer.fillRect(leftPad, y, pageWidth - 2 * leftPad, 28, true);
    renderer.drawCenteredText(UI_10_FONT_ID, y + 4, "ALERT: ANOMALOUS FRAME RATE DETECTED", false,
                              EpdFontFamily::BOLD);
    y += 34;
  }

  // Frame rate text
  char rateBuf[32];
  snprintf(rateBuf, sizeof(rateBuf), "%lu frames/sec",
           static_cast<unsigned long>(framesPerSec));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, rateBuf, true, EpdFontFamily::BOLD);
  y += renderer.getTextHeight(UI_10_FONT_ID) + 6;

  // ---- Rate history bar graph ----
  const int graphX      = leftPad + 20;
  const int graphWidth  = pageWidth - graphX - 20;
  const int graphHeight = 60;
  const int graphY      = y;

  uint16_t maxRate = 1;
  for (int i = 0; i < GRAPH_POINTS; i++) {
    if (rateHistory[i] > maxRate) maxRate = rateHistory[i];
  }

  const int barW = graphWidth / GRAPH_POINTS;
  for (int i = 0; i < GRAPH_POINTS; i++) {
    const int idx = (historyIndex + i) % GRAPH_POINTS;
    const int barH = (static_cast<int>(rateHistory[idx]) * graphHeight) / maxRate;
    if (barH > 0) {
      renderer.fillRect(graphX + i * barW,
                        graphY + graphHeight - barH,
                        (barW > 2 ? barW - 1 : 1),
                        barH, true);
    }
  }
  renderer.drawRect(graphX - 1, graphY - 1, graphWidth + 2, graphHeight + 2, true);
  y += graphHeight + 10;

  // Total frames and source count
  portENTER_CRITICAL(&dataMux);
  const uint32_t capturedTotal  = totalFrames;
  const int      capturedCount  = eventCount;
  portEXIT_CRITICAL(&dataMux);

  char statBuf[64];
  snprintf(statBuf, sizeof(statBuf), "Total: %lu  Sources: %d",
           static_cast<unsigned long>(capturedTotal), capturedCount);
  renderer.drawText(UI_10_FONT_ID, leftPad, y, statBuf);
  y += renderer.getTextHeight(UI_10_FONT_ID) + 8;

  // ---- Event list ----
  const int listTop    = y;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int listHeight = listBottom - listTop;

  if (capturedCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listHeight / 2,
                              "No anomalous frames detected");
  } else {
    if (selectorIndex >= capturedCount) {
      selectorIndex = capturedCount - 1;
    }

    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight},
        capturedCount, selectorIndex,
        [this](int i) -> std::string {
          portENTER_CRITICAL(&dataMux);
          const uint8_t atkMac[6] = {
              events[i].attackerMac[0], events[i].attackerMac[1],
              events[i].attackerMac[2], events[i].attackerMac[3],
              events[i].attackerMac[4], events[i].attackerMac[5]};
          const uint8_t tgtBssid[6] = {
              events[i].targetBssid[0], events[i].targetBssid[1],
              events[i].targetBssid[2], events[i].targetBssid[3],
              events[i].targetBssid[4], events[i].targetBssid[5]};
          portEXIT_CRITICAL(&dataMux);
          char buf[48];
          snprintf(buf, sizeof(buf), "%02X:%02X:%02X > %02X:%02X:%02X",
                   atkMac[0], atkMac[1], atkMac[2],
                   tgtBssid[0], tgtBssid[1], tgtBssid[2]);
          return std::string(buf);
        },
        [this](int i) -> std::string {
          portENTER_CRITICAL(&dataMux);
          const uint32_t cnt  = events[i].count;
          const uint8_t  ch   = events[i].channel;
          const int8_t   rssi = events[i].rssi;
          portEXIT_CRITICAL(&dataMux);
          char buf[40];
          snprintf(buf, sizeof(buf), "%lux Ch%d %ddBm",
                   static_cast<unsigned long>(cnt),
                   static_cast<int>(ch),
                   static_cast<int>(rssi));
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels("Exit", "Detail", "Ch-", "Ch+");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold:AP Scan", labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// renderRogueScanning
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::renderRogueScanning() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Network Monitor", "AP Scan");

  char buf[48];
  snprintf(buf, sizeof(buf), "Scanning... (Phase %d/%d)", rogueScanPhase + 1, ROGUE_SCAN_PHASES);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, buf);

  char countBuf[32];
  snprintf(countBuf, sizeof(countBuf), "%d APs found so far", static_cast<int>(allAps.size()));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, countBuf);
}

// ---------------------------------------------------------------------------
// renderRogueResults
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::renderRogueResults() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  char subtitle[48];
  snprintf(subtitle, sizeof(subtitle), "%d APs, %d suspicious",
           static_cast<int>(allAps.size()), suspiciousCount);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Network Monitor", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (ssidGroups.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No networks found");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(ssidGroups.size()), rogueSelectorIndex,
        [this](int index) -> std::string {
          const auto& g = ssidGroups[index];
          std::string title;
          if (g.suspicious) title = "! ";
          title += g.ssid;
          return title;
        },
        [this](int index) -> std::string {
          const auto& g = ssidGroups[index];
          char buf[64];
          snprintf(buf, sizeof(buf), "%d APs", g.apCount);
          std::string sub = buf;
          if (g.mixedEncryption) sub += " MIXED ENC!";
          if (g.mixedChannels)   sub += " Multi-Ch";
          return sub;
        });
  }

  const auto labels = mappedInput.mapLabels("Exit", "Detail", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold:Frame Mon", labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// renderRogueDetail
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::renderRogueDetail() {
  if (rogueDetailGroupIndex < 0 ||
      rogueDetailGroupIndex >= static_cast<int>(ssidGroups.size())) {
    state = ROGUE_RESULTS;
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int leftPad = metrics.contentSidePadding;

  const SsidGroup& g = ssidGroups[rogueDetailGroupIndex];

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 g.ssid.c_str());

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 10;

  // Risk assessment
  const char* riskLabel;
  if (g.mixedEncryption) {
    riskLabel = "RISK: HIGH";
  } else if (g.suspicious) {
    riskLabel = "RISK: MEDIUM";
  } else {
    riskLabel = "RISK: LOW";
  }

  renderer.drawText(UI_12_FONT_ID, leftPad, y, riskLabel, true, EpdFontFamily::BOLD);
  y += 40;

  // Reason line
  if (g.mixedEncryption) {
    renderer.drawText(SMALL_FONT_ID, leftPad, y, "Multiple BSSIDs + mixed encryption detected");
  } else if (g.suspicious) {
    renderer.drawText(SMALL_FONT_ID, leftPad, y, "Multiple BSSIDs sharing this SSID");
  } else {
    renderer.drawText(SMALL_FONT_ID, leftPad, y, "Single AP — no anomaly detected");
  }
  y += 28;

  // Separator
  renderer.fillRect(leftPad, y, pageWidth - leftPad * 2, 1, true);
  y += 10;

  // List all APs in this group
  const int lineH = 38;
  const int bottomLimit = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - lineH;

  for (const auto& ap : allAps) {
    if (ap.ssid != g.ssid) continue;
    if (y >= bottomLimit) break;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s  Ch%d  %ddBm  %s",
             ap.bssid.c_str(),
             static_cast<int>(ap.channel),
             static_cast<int>(ap.rssi),
             encryptionString(ap.encType));
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineH;
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------------------------------------------------------------------------
// saveToCsv
// ---------------------------------------------------------------------------

void NetworkMonitorActivity::saveToCsv() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");

  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/logs/netmon_%lu.csv", millis());

  String csv = "SourceMAC,TargetBSSID,Channel,Type,Reason,Count,RSSI\n";

  portENTER_CRITICAL(&dataMux);
  const int count = eventCount;
  for (int i = 0; i < count; i++) {
    char line[128];
    snprintf(line, sizeof(line),
             "%02X:%02X:%02X:%02X:%02X:%02X,"
             "%02X:%02X:%02X:%02X:%02X:%02X,"
             "%d,%s,%d,%lu,%d\n",
             events[i].attackerMac[0], events[i].attackerMac[1],
             events[i].attackerMac[2], events[i].attackerMac[3],
             events[i].attackerMac[4], events[i].attackerMac[5],
             events[i].targetBssid[0], events[i].targetBssid[1],
             events[i].targetBssid[2], events[i].targetBssid[3],
             events[i].targetBssid[4], events[i].targetBssid[5],
             static_cast<int>(events[i].channel),
             (events[i].frameType == 0xA0) ? "Disassoc" : "MgmtFrame",
             static_cast<int>(events[i].reasonCode),
             static_cast<unsigned long>(events[i].count),
             static_cast<int>(events[i].rssi));
    csv += line;
  }
  portEXIT_CRITICAL(&dataMux);

  Storage.writeFile(filename, csv);
  LOG_DBG("NETMON", "Saved %d events to %s", count, filename);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string NetworkMonitorActivity::macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

const char* NetworkMonitorActivity::reasonCodeStr(uint8_t code) {
  switch (code) {
    case 1: return "Unspecified";
    case 2: return "Auth expired";
    case 3: return "STA leaving";
    case 4: return "Inactivity";
    case 5: return "AP full";
    case 6: return "Class 2 err";
    case 7: return "Class 3 err";
    case 8: return "STA leaving BSS";
    default: return "Other";
  }
}

const char* NetworkMonitorActivity::encryptionString(uint8_t type) {
  switch (type) {
    case WIFI_AUTH_OPEN:          return "Open";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default:                      return "?";
  }
}
