#include "HostScannerActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

constexpr uint16_t HostScannerActivity::COMMON_PORTS[];

std::string HostScannerActivity::ipToString(uint32_t ip) const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return buf;
}

void HostScannerActivity::onEnter() {
  Activity::onEnter();
  state = CHECKING_WIFI;
  hosts.clear();
  selectorIndex = 0;
  scanProgress = 0;
  selectedHost = -1;

  if (WiFi.isConnected()) {
    RADIO.ensureWifi();  // Sync RadioManager state with actual radio
    startHostScan();
  }
  requestUpdate();
}

void HostScannerActivity::onExit() {
  Activity::onExit();
  RADIO.shutdown();
}

void HostScannerActivity::startHostScan() {
  state = SCANNING_HOSTS;
  hosts.clear();
  scanProgress = 0;

  uint32_t localIp = WiFi.localIP();
  uint32_t mask = WiFi.subnetMask();
  uint32_t network = localIp & mask;
  scanTotal = (~mask & 0xFF) - 1;  // Number of host addresses
  if (scanTotal > 254) scanTotal = 254;
}

void HostScannerActivity::scanNextHost() {
  if (scanProgress >= scanTotal) {
    state = HOST_LIST;
    requestUpdate();
    return;
  }

  scanProgress++;
  uint32_t localIp = WiFi.localIP();
  uint32_t mask = WiFi.subnetMask();
  uint32_t network = localIp & mask;
  uint32_t targetIp = network | (scanProgress & 0xFF);

  // Skip own IP
  if (targetIp == localIp) return;

  // Try TCP connect to port 80 with short timeout
  WiFiClient client;
  client.setTimeout(200);
  IPAddress addr(targetIp);

  if (client.connect(addr, 80)) {
    client.stop();
    Host host;
    host.ip = targetIp;
    hosts.push_back(host);
    requestUpdate();
  }

  if (scanProgress % 10 == 0) requestUpdate();
}

void HostScannerActivity::startPortScan(int hostIndex) {
  selectedHost = hostIndex;
  portScanProgress = 0;
  hosts[hostIndex].openPorts.clear();
  state = PORT_SCANNING;
}

void HostScannerActivity::scanNextPort() {
  if (portScanProgress >= NUM_PORTS) {
    state = PORT_RESULTS;
    requestUpdate();
    return;
  }

  uint16_t port = COMMON_PORTS[portScanProgress];
  portScanProgress++;

  WiFiClient client;
  client.setTimeout(1000);
  IPAddress addr(hosts[selectedHost].ip);

  if (client.connect(addr, port)) {
    client.stop();
    hosts[selectedHost].openPorts.push_back(port);
  }

  requestUpdate();
}

void HostScannerActivity::saveToCsv() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/scans");
  char filename[64];
  snprintf(filename, sizeof(filename), "/biscuit/scans/hostscan_%lu.csv", millis());

  String csv = "IP,Open Ports\n";
  for (const auto& host : hosts) {
    csv += ipToString(host.ip).c_str();
    csv += ",";
    for (size_t i = 0; i < host.openPorts.size(); i++) {
      if (i > 0) csv += ";";
      csv += String(host.openPorts[i]);
    }
    csv += "\n";
  }
  Storage.writeFile(filename, csv);
  LOG_DBG("HSCAN", "Saved to %s", filename);
}

void HostScannerActivity::loop() {
  if (state == CHECKING_WIFI) {
    if (!WiFi.isConnected()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    } else {
      startHostScan();
    }
    return;
  }

  if (state == SCANNING_HOSTS) {
    scanNextHost();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = HOST_LIST;
      requestUpdate();
    }
    return;
  }

  if (state == PORT_SCANNING) {
    scanNextPort();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = HOST_LIST;
      requestUpdate();
    }
    return;
  }

  if (state == PORT_RESULTS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = HOST_LIST;
      requestUpdate();
    }
    return;
  }

  // HOST_LIST
  const int count = static_cast<int>(hosts.size());

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
    } else if (!hosts.empty()) {
      startPortScan(selectorIndex);
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void HostScannerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HOST_SCANNER));

  if (state == CHECKING_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NOT_CONNECTED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SCANNING_HOSTS) {
    std::string progress = tr(STR_SCANNING_HOSTS) + std::string(" ") + std::to_string(scanProgress) + "/" +
                           std::to_string(scanTotal);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, progress.c_str());
    std::string found = std::to_string(hosts.size()) + " hosts found";
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, found.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == PORT_SCANNING) {
    std::string ip = ipToString(hosts[selectedHost].ip);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, ip.c_str(), true, EpdFontFamily::BOLD);
    std::string progress =
        "Port " + std::to_string(portScanProgress) + "/" + std::to_string(NUM_PORTS);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, progress.c_str());
    std::string open = std::to_string(hosts[selectedHost].openPorts.size()) + " open";
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, open.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == PORT_RESULTS && selectedHost >= 0) {
    const auto& host = hosts[selectedHost];
    int y = metrics.topPadding + metrics.headerHeight + 30;
    const int leftPad = metrics.contentSidePadding;

    renderer.drawText(UI_10_FONT_ID, leftPad, y, ipToString(host.ip).c_str(), true, EpdFontFamily::BOLD);
    y += 40;

    renderer.drawText(SMALL_FONT_ID, leftPad, y, tr(STR_OPEN_PORTS), true, EpdFontFamily::BOLD);
    y += 25;

    if (host.openPorts.empty()) {
      renderer.drawText(UI_10_FONT_ID, leftPad, y, "None");
    } else {
      for (uint16_t port : host.openPorts) {
        renderer.drawText(UI_10_FONT_ID, leftPad, y, std::to_string(port).c_str());
        y += 25;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // HOST_LIST
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (hosts.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_HOSTS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(hosts.size()), selectorIndex,
        [this](int i) -> std::string { return ipToString(hosts[i].ip); }, nullptr);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold: CSV", labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
