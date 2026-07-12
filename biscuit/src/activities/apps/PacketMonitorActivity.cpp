#include "PacketMonitorActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "util/RadioManager.h"

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Static instance pointer for C callback
static PacketMonitorActivity* activeMonitor = nullptr;

static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeMonitor || !buf) return;
  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  activeMonitor->onPacket(pkt->payload, pkt->rx_ctrl.sig_len, pkt->rx_ctrl.rssi);
}

void PacketMonitorActivity::onPacket(const uint8_t* buf, uint16_t len, int rssi) {
  portENTER_CRITICAL(&statsMux);
  totalPackets = totalPackets + 1;
  intervalPackets = intervalPackets + 1;

  if (currentChannel >= 1 && currentChannel <= 13) {
    channelPackets[currentChannel]++;
  }

  // Extract source MAC from 802.11 header (offset 10)
  // Use simple hash table for deduplication (no heap alloc in callback)
  if (len >= 16) {
    uint64_t mac = 0;
    for (int i = 0; i < 6; i++) {
      mac = (mac << 8) | buf[10 + i];
    }
    // Linear probing hash table insert
    uint32_t idx = static_cast<uint32_t>(mac) % MAC_TABLE_SIZE;
    for (int probe = 0; probe < MAC_TABLE_SIZE; probe++) {
      uint32_t pos = (idx + probe) % MAC_TABLE_SIZE;
      if (macTable[pos] == 0) {
        macTable[pos] = mac;
        uniqueMacCount = uniqueMacCount + 1;
        break;
      }
      if (macTable[pos] == mac) break;  // already seen
    }
  }
  portEXIT_CRITICAL(&statsMux);

  // Write to PCAP file if recording
  if (pcapRecording && pcapFileOpen) {
    if (captureMode == CAPTURE_EAPOL_ONLY && !isEapolPacket(buf, len)) return;
    if (isEapolPacket(buf, len)) eapolFound = true;
    if (xSemaphoreTake(fileMux, pdMS_TO_TICKS(50)) == pdTRUE) {
      writePcapPacket(buf, len);
      xSemaphoreGive(fileMux);
    }
  }
}

void PacketMonitorActivity::onEnter() {
  Activity::onEnter();
  if (!fileMux) fileMux = xSemaphoreCreateMutex();
  totalPackets = 0;
  intervalPackets = 0;
  packetsPerSec = 0;
  currentChannel = 1;
  autoHop = true;
  memset(macTable, 0, sizeof(macTable));
  uniqueMacCount = 0;
  memset(channelPackets, 0, sizeof(channelPackets));
  lastUpdateTime = millis();
  lastHopTime = millis();

  // Reset recording state
  pcapRecording = false;
  pcapFileOpen = false;
  packetsSaved = 0;
  pcapFileSize = 0;
  eapolFound = false;

  startMonitor();
  requestUpdate();
}

void PacketMonitorActivity::onExit() {
  Activity::onExit();
  stopPcapRecording();
  stopMonitor();
  if (fileMux) {
    vSemaphoreDelete(fileMux);
    fileMux = nullptr;
  }
}

void PacketMonitorActivity::startMonitor() {
  RADIO.ensureWifi();
  WiFi.disconnect();

  activeMonitor = this;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
  setChannel(currentChannel);
  monitoring = true;
}

void PacketMonitorActivity::stopMonitor() {
  monitoring = false;
  esp_wifi_set_promiscuous(false);
  activeMonitor = nullptr;
  RADIO.shutdown();
}

void PacketMonitorActivity::setChannel(uint8_t ch) {
  if (ch < 1) ch = 1;
  if (ch > 13) ch = 13;
  currentChannel = ch;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

void PacketMonitorActivity::saveToCsv() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");
  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/logs/pktmon_%lu.csv", millis());

  String csv = "Channel,Packets\n";
  for (int ch = 1; ch <= 13; ch++) {
    csv += String(ch) + "," + String(channelPackets[ch]) + "\n";
  }
  csv += "\nTotal Packets," + String(totalPackets) + "\n";
  csv += "Unique MACs," + String(uniqueMacCount) + "\n";
  Storage.writeFile(filename, csv);
  LOG_DBG("PKTMON", "Saved stats to %s", filename);
}

void PacketMonitorActivity::startPcapRecording() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/pcap");
  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/pcap/capture_%lu.pcap", millis());
  pcapFile = Storage.open(filename, O_WRITE | O_CREAT | O_TRUNC);
  if (!pcapFile) {
    LOG_ERR("PKTMON", "Failed to open PCAP file");
    return;
  }
  pcapFileOpen = true;
  packetsSaved = 0;
  pcapFileSize = 0;
  eapolFound = false;
  writePcapHeader();
  pcapRecording = true;
}

void PacketMonitorActivity::stopPcapRecording() {
  pcapRecording = false;
  if (fileMux && xSemaphoreTake(fileMux, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (pcapFileOpen) {
      pcapFile.flush();
      pcapFile.close();
      pcapFileOpen = false;
    }
    xSemaphoreGive(fileMux);
  }
}

void PacketMonitorActivity::writePcapHeader() {
  // PCAP global header
  struct {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t version_major = 2;
    uint16_t version_minor = 4;
    int32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen = 65535;
    uint32_t network = 105;  // IEEE 802.11
  } __attribute__((packed)) header;

  pcapFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  pcapFileSize = sizeof(header);
}

void PacketMonitorActivity::writePcapPacket(const uint8_t* data, uint16_t len) {
  // PCAP packet header
  uint32_t ts = millis() / 1000;
  uint32_t ts_usec = (millis() % 1000) * 1000;

  struct {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
  } __attribute__((packed)) pktHeader = {ts, ts_usec, len, len};

  pcapFile.write(reinterpret_cast<const uint8_t*>(&pktHeader), sizeof(pktHeader));
  pcapFile.write(data, len);
  pcapFileSize = pcapFileSize + sizeof(pktHeader) + len;
  packetsSaved = packetsSaved + 1;
}

bool PacketMonitorActivity::isEapolPacket(const uint8_t* data, uint16_t len) const {
  if (len < 36) return false;
  // Check both standard data frame offset (24+6=30) and QoS data frame offset (26+6=32)
  // LLC/SNAP header ends with ethertype at offset 30-31 or 32-33
  for (int i = 30; i <= 33 && i + 1 < len; i++) {
    if (data[i] == 0x88 && data[i + 1] == 0x8E) return true;
  }
  return false;
}

void PacketMonitorActivity::loop() {
  if (!monitoring) return;

  unsigned long now = millis();

  // Channel hopping
  if (autoHop && (now - lastHopTime >= HOP_INTERVAL_MS)) {
    lastHopTime = now;
    uint8_t nextCh = (currentChannel % 13) + 1;
    setChannel(nextCh);
  }

  // Update display periodically
  if (now - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    packetsPerSec = (intervalPackets * 1000) / (now - lastUpdateTime);
    intervalPackets = 0;
    lastUpdateTime = now;
    requestUpdate();
  }

  // Manual channel control
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    autoHop = false;
    if (currentChannel > 1) setChannel(currentChannel - 1);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    autoHop = false;
    if (currentChannel < 13) setChannel(currentChannel + 1);
    requestUpdate();
  }

  // Toggle PCAP recording (long press) or auto-hop (short press)
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= 500) {
      if (!pcapRecording) {
        startPcapRecording();
      } else {
        stopPcapRecording();
      }
    } else {
      autoHop = !autoHop;
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void PacketMonitorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  std::string chInfo = std::string(tr(STR_CHANNEL)) + " " + std::to_string(currentChannel) +
                       (autoHop ? " (auto)" : " (manual)");
  if (pcapRecording) chInfo += " REC";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PACKET_MONITOR),
                 chInfo.c_str());

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 10;
  const int fontH = renderer.getTextHeight(UI_10_FONT_ID);

  // Stats
  std::string pktStr = std::string(tr(STR_PKTS_SEC)) + ": " + std::to_string(packetsPerSec);
  renderer.drawText(UI_10_FONT_ID, leftPad, y, pktStr.c_str(), true, EpdFontFamily::BOLD);
  y += fontH + 10;

  std::string totalStr = std::string(tr(STR_TOTAL_PKTS)) + ": " + std::to_string(totalPackets);
  renderer.drawText(UI_10_FONT_ID, leftPad, y, totalStr.c_str());
  y += fontH + 10;

  std::string macStr = std::string(tr(STR_UNIQUE_MACS)) + ": " + std::to_string(static_cast<uint32_t>(uniqueMacCount));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, macStr.c_str());
  y += fontH + 30;

  // Channel bar chart
  const int chartX = leftPad;
  const int chartWidth = pageWidth - 2 * leftPad;
  const int chartHeight = 200;
  const int barWidth = chartWidth / 13;

  // Find max for scaling
  uint32_t maxPkts = 1;
  for (int ch = 1; ch <= 13; ch++) {
    if (channelPackets[ch] > maxPkts) maxPkts = channelPackets[ch];
  }

  // Draw bars
  for (int ch = 1; ch <= 13; ch++) {
    int bx = chartX + (ch - 1) * barWidth;
    int barH = (channelPackets[ch] * chartHeight) / maxPkts;
    if (barH < 2 && channelPackets[ch] > 0) barH = 2;

    // Bar
    renderer.fillRect(bx + 2, y + chartHeight - barH, barWidth - 4, barH, true);

    // Highlight current channel
    if (ch == currentChannel) {
      renderer.drawRect(bx, y, barWidth, chartHeight, true);
    }

    // Channel label
    char label[4];
    snprintf(label, sizeof(label), "%d", ch);
    int tw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, bx + (barWidth - tw) / 2, y + chartHeight + 3, label);
  }

  y += chartHeight + fontH + 10;

  // Recording info
  if (pcapRecording) {
    char recStr[48];
    snprintf(recStr, sizeof(recStr), "REC: %lu packets, %lu KB",
             static_cast<uint32_t>(packetsSaved),
             static_cast<uint32_t>(pcapFileSize) / 1024);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, recStr, true, EpdFontFamily::BOLD);
    y += fontH + 6;
    if (eapolFound) {
      renderer.drawText(UI_10_FONT_ID, leftPad, y, "EAPOL!", true, EpdFontFamily::BOLD);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), autoHop ? "Manual" : "Auto", "Ch-", "Ch+");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold: REC", labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
