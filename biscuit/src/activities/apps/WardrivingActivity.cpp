#include "WardrivingActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void WardrivingActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  WiFi.disconnect();
  logging = false;
  networks.clear();
  scanCount = 0;
  totalNewThisScan = 0;
  selectorIndex = 0;
  filename.clear();
  requestUpdate();
}

void WardrivingActivity::onExit() {
  Activity::onExit();
  stopLogging();
  RADIO.shutdown();
}

void WardrivingActivity::startLogging() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");

  char buf[64];
  snprintf(buf, sizeof(buf), "/biscuit/logs/wardrive_%lu.csv", millis());
  filename = buf;

  Storage.writeFile(filename.c_str(), "SSID,BSSID,RSSI,Channel,Encryption,FirstSeen_ms\n");

  startTime = millis();
  logging = true;
  scanCount = 0;
  totalNewThisScan = 0;

  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async
  lastScanTime = millis();

  LOG_DBG("WARD", "Started logging to %s", filename.c_str());
}

void WardrivingActivity::stopLogging() {
  if (!logging) return;
  WiFi.scanDelete();
  logging = false;
  LOG_DBG("WARD", "Stopped logging. %zu unique networks.", networks.size());
}

void WardrivingActivity::appendNetworkToCsv(const SeenNetwork& net) {
  if (filename.empty()) return;
  FsFile f = Storage.open(filename.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (f) {
    String line = String(net.ssid.c_str()) + "," + net.bssid.c_str() + "," + String(net.rssi) + "," +
                  String(net.channel) + "," + encryptionString(net.encType) + "," + String(net.firstSeen) + "\n";
    f.print(line);
    f.close();
  }
}

void WardrivingActivity::processScanResults() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  int newThisScan = 0;

  if (result > 0) {
    for (int i = 0; i < result; i++) {
      uint8_t* rawBssid = WiFi.BSSID(i);
      char bssidBuf[20];
      snprintf(bssidBuf, sizeof(bssidBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
               rawBssid[0], rawBssid[1], rawBssid[2], rawBssid[3], rawBssid[4], rawBssid[5]);

      // Check if BSSID already seen
      bool found = false;
      for (auto& net : networks) {
        if (net.bssid == bssidBuf) {
          net.lastSeen = millis();
          net.rssi = WiFi.RSSI(i);
          found = true;
          break;
        }
      }

      if (!found && static_cast<int>(networks.size()) < MAX_NETWORKS) {
        SeenNetwork net;
        String ssid = WiFi.SSID(i);
        net.ssid = ssid.isEmpty() ? "(hidden)" : ssid.c_str();
        net.bssid = bssidBuf;
        net.rssi = WiFi.RSSI(i);
        net.channel = static_cast<uint8_t>(WiFi.channel(i));
        net.encType = static_cast<uint8_t>(WiFi.encryptionType(i));
        net.firstSeen = millis();
        net.lastSeen = net.firstSeen;

        if (logging) appendNetworkToCsv(net);
        networks.push_back(std::move(net));
        newThisScan++;
      }
    }
  }

  WiFi.scanDelete();
  scanCount++;
  totalNewThisScan = newThisScan;
  lastScanTime = millis();
  requestUpdate();
}

const char* WardrivingActivity::encryptionString(uint8_t type) {
  switch (type) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default: return "?";
  }
}

void WardrivingActivity::loop() {
  if (logging) {
    processScanResults();

    // Advance spinner while waiting for initial scan results
    if (networks.empty()) {
      if (millis() - lastSpinnerUpdate >= 600) {
        lastSpinnerUpdate = millis();
        spinnerFrame = (spinnerFrame + 1) % 3;
        requestUpdate();
      }
    }

    // Start next async scan after interval, but only if the previous one finished
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
        millis() - lastScanTime >= SCAN_INTERVAL_MS) {
      WiFi.scanNetworks(true);
    }
  }

  const int count = static_cast<int>(networks.size());

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
    if (logging) {
      stopLogging();
    } else {
      networks.clear();
      selectorIndex = 0;
      startLogging();
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (logging) stopLogging();
    finish();
  }
}

void WardrivingActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Build subtitle: "N unique | scan #M"  or  "N unique"
  char subtitleBuf[48];
  if (logging) {
    snprintf(subtitleBuf, sizeof(subtitleBuf), "%zu unique | scan #%d", networks.size(), scanCount);
  } else {
    snprintf(subtitleBuf, sizeof(subtitleBuf), "%zu unique", networks.size());
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Wardriving", subtitleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (networks.empty()) {
    if (logging) {
      GUI.drawSpinner(renderer, pageWidth / 2, pageHeight / 2, "SCANNING...", spinnerFrame);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Press OK to start");
    }
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(networks.size()), selectorIndex,
        [this](int index) -> std::string {
          return networks[index].ssid;
        },
        [this](int index) -> std::string {
          const auto& net = networks[index];
          char buf[40];
          snprintf(buf, sizeof(buf), "%d dBm  Ch%u  %s",
                   net.rssi, net.channel, encryptionString(net.encType));
          return buf;
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), logging ? "Stop" : "Start",
                                            "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
