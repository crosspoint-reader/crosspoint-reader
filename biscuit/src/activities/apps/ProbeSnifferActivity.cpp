#include "ProbeSnifferActivity.h"

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

static ProbeSnifferActivity* activeSniffer = nullptr;

static void probeSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeSniffer || !buf) return;
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = pkt->payload;
  const uint16_t sig_len = pkt->rx_ctrl.sig_len;

  if (sig_len < 28) return;

  // Frame type check: probe request = 0x40 (type 0, subtype 4)
  if ((payload[0] & 0xFC) != 0x40) return;

  // Source MAC is at bytes 10-15
  const uint8_t* srcMac = payload + 10;

  // Tagged parameters start at offset 24
  // SSID tag: tag number (byte), length (byte), then SSID bytes
  char ssid[33] = {0};
  if (sig_len >= 26) {
    const uint8_t tag = payload[24];
    const uint8_t tagLen = payload[25];
    if (tag == 0 && tagLen > 0 && tagLen <= 32 && static_cast<uint16_t>(26 + tagLen) <= sig_len) {
      memcpy(ssid, payload + 26, tagLen);
      ssid[tagLen] = '\0';
    }
  }

  activeSniffer->onProbeRequest(srcMac, ssid, pkt->rx_ctrl.rssi);
}

void ProbeSnifferActivity::onProbeRequest(const uint8_t* srcMac, const char* ssid, int rssi) {
  portENTER_CRITICAL(&dataMux);

  // Search for an existing entry matching both MAC and SSID
  for (auto& entry : entries) {
    if (memcmp(entry.mac, srcMac, 6) == 0 && entry.ssid == ssid) {
      entry.count++;
      entry.rssi = rssi;
      entry.lastSeen = millis();
      portEXIT_CRITICAL(&dataMux);
      return;
    }
  }

  // New entry
  if (static_cast<int>(entries.size()) < MAX_ENTRIES) {
    ProbeEntry e;
    memcpy(e.mac, srcMac, 6);
    e.ssid = ssid;
    e.rssi = rssi;
    e.count = 1;
    e.lastSeen = millis();
    entries.push_back(std::move(e));
  }

  portEXIT_CRITICAL(&dataMux);
}

void ProbeSnifferActivity::onEnter() {
  Activity::onEnter();

  state = SNIFFING_VIEW;
  entries.clear();
  entries.reserve(MAX_ENTRIES);
  selectorIndex = 0;
  detailIndex = 0;
  sniffing = false;
  lastUpdateTime = 0;
  lastHopTime = 0;
  currentChannel = 1;

  RADIO.ensureWifi();
  activeSniffer = this;
  startSniffing();
  requestUpdate();
}

void ProbeSnifferActivity::onExit() {
  Activity::onExit();
  stopSniffing();
  activeSniffer = nullptr;
  RADIO.shutdown();
}

void ProbeSnifferActivity::startSniffing() {
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(probeSnifferCallback);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  currentChannel = 1;
  sniffing = true;
  lastHopTime = millis();
  lastUpdateTime = millis();
  LOG_DBG("PROBE", "Sniffing started");
}

void ProbeSnifferActivity::stopSniffing() {
  esp_wifi_set_promiscuous(false);
  sniffing = false;
  LOG_DBG("PROBE", "Sniffing stopped, %zu entries", entries.size());
}

void ProbeSnifferActivity::loop() {
  if (state == DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = SNIFFING_VIEW;
      requestUpdate();
    }
    return;
  }

  // SNIFFING_VIEW
  unsigned long now = millis();

  // Advance spinner while waiting for first probe requests
  if (entries.empty()) {
    if (now - lastSpinnerUpdate >= 600) {
      lastSpinnerUpdate = now;
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
    }
  }

  // Channel hop
  if (sniffing && (now - lastHopTime >= HOP_INTERVAL_MS)) {
    lastHopTime = now;
    currentChannel = static_cast<uint8_t>((currentChannel % 13) + 1);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  }

  // Periodic display refresh: sort by lastSeen descending
  if (now - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    lastUpdateTime = now;
    portENTER_CRITICAL(&dataMux);
    auto sortCopy = entries;
    portEXIT_CRITICAL(&dataMux);
    std::sort(sortCopy.begin(), sortCopy.end(),
              [](const ProbeEntry& a, const ProbeEntry& b) { return a.lastSeen > b.lastSeen; });
    portENTER_CRITICAL(&dataMux);
    entries = std::move(sortCopy);
    portEXIT_CRITICAL(&dataMux);
    requestUpdate();
  }

  // Up/Down navigation
  const int count = static_cast<int>(entries.size());
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
    if (mappedInput.getHeldTime() >= 500) {
      saveToCsv();
    } else if (!entries.empty()) {
      detailIndex = selectorIndex;
      state = DETAIL;
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ProbeSnifferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (state == DETAIL) {
    if (detailIndex >= static_cast<int>(entries.size())) {
      // Guard against stale index
      state = SNIFFING_VIEW;
    } else {
      portENTER_CRITICAL(&dataMux);
      const ProbeEntry entry = entries[detailIndex];  // copy under lock
      portEXIT_CRITICAL(&dataMux);

      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Probe Detail");

      const int leftPad = metrics.contentSidePadding;
      int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
      const int lineH = 45;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "MAC", true, EpdFontFamily::BOLD);
      y += 22;
      std::string macStr = macToString(entry.mac);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, macStr.c_str());
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "SSID", true, EpdFontFamily::BOLD);
      y += 22;
      renderer.drawText(UI_10_FONT_ID, leftPad, y,
                        entry.ssid.empty() ? "(broadcast)" : entry.ssid.c_str());
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, tr(STR_RSSI), true, EpdFontFamily::BOLD);
      y += 22;
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%d dBm", entry.rssi);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, rssiStr);
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Count", true, EpdFontFamily::BOLD);
      y += 22;
      char countStr[16];
      snprintf(countStr, sizeof(countStr), "%lu", static_cast<unsigned long>(entry.count));
      renderer.drawText(UI_10_FONT_ID, leftPad, y, countStr);
      y += lineH;

      renderer.drawText(SMALL_FONT_ID, leftPad, y, "Last Seen", true, EpdFontFamily::BOLD);
      y += 22;
      char lastSeenStr[20];
      unsigned long ageSec = (millis() - entry.lastSeen) / 1000UL;
      snprintf(lastSeenStr, sizeof(lastSeenStr), "%lus ago", ageSec);
      renderer.drawText(UI_10_FONT_ID, leftPad, y, lastSeenStr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }
  }

  // SNIFFING_VIEW
  portENTER_CRITICAL(&dataMux);
  const int entryCount = static_cast<int>(entries.size());
  portEXIT_CRITICAL(&dataMux);

  char subtitle[24];
  snprintf(subtitle, sizeof(subtitle), "%d probes", entryCount);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Probe Sniffer", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (entryCount == 0) {
    GUI.drawSpinner(renderer, pageWidth / 2, pageHeight / 2, "SCANNING...", spinnerFrame);
  } else {
    // Clamp selectorIndex defensively
    if (selectorIndex >= entryCount) selectorIndex = entryCount - 1;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, entryCount, selectorIndex,
        [this](int i) -> std::string {
          portENTER_CRITICAL(&dataMux);
          const std::string ssid = entries[i].ssid;
          portEXIT_CRITICAL(&dataMux);
          return ssid.empty() ? "(broadcast)" : ssid;
        },
        [this](int i) -> std::string {
          portENTER_CRITICAL(&dataMux);
          const uint8_t mac[6] = {entries[i].mac[0], entries[i].mac[1], entries[i].mac[2],
                                  entries[i].mac[3], entries[i].mac[4], entries[i].mac[5]};
          const int rssi = entries[i].rssi;
          const uint32_t count = entries[i].count;
          portEXIT_CRITICAL(&dataMux);
          char buf[48];
          snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X %ddBm %lux",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                   rssi, static_cast<unsigned long>(count));
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold:CSV", labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ProbeSnifferActivity::saveToCsv() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");

  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/logs/probes_%lu.csv", millis());

  String csv = "MAC,SSID,RSSI,Count\n";

  portENTER_CRITICAL(&dataMux);
  auto entriesCopy = entries;
  portEXIT_CRITICAL(&dataMux);

  const size_t count = entriesCopy.size();
  for (size_t i = 0; i < count; i++) {
    char line[96];
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%lu\n",
             entriesCopy[i].mac[0], entriesCopy[i].mac[1], entriesCopy[i].mac[2],
             entriesCopy[i].mac[3], entriesCopy[i].mac[4], entriesCopy[i].mac[5],
             entriesCopy[i].ssid.c_str(), entriesCopy[i].rssi,
             static_cast<unsigned long>(entriesCopy[i].count));
    csv += line;
  }

  Storage.writeFile(filename, csv);
  LOG_DBG("PROBE", "Saved %zu probes to %s", count, filename);
}

std::string ProbeSnifferActivity::macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}
