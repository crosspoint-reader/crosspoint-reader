#include "SweepActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <Logging.h>
#include <WiFi.h>

#include <cctype>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// Known camera SSID substrings (uppercase for case-insensitive match)
static const char* const CAMERA_PATTERNS[] = {
    "IPCAM", "HIKAM", "YI-", "EZVIZ", "WYZE", "TAPO", "REOLINK", "HIKVISION", "DAHUA"};
static constexpr int CAMERA_PATTERN_COUNT = 9;

// Known skimmer OUI prefixes (first 3 bytes)
struct SkimmerOui { uint8_t b0, b1, b2; };
static const SkimmerOui SKIMMER_OUIS[] = {
    {0x00, 0x14, 0x03},
    {0x20, 0x15, 0x05},
    {0x98, 0xD3, 0x31},
};
static constexpr int SKIMMER_OUI_COUNT = 3;

// ---- helpers ----------------------------------------------------------------

static void toUpperBuf(const char* src, char* dst, size_t dstLen) {
  size_t i = 0;
  for (; src[i] && i < dstLen - 1; i++) dst[i] = static_cast<char>(toupper(static_cast<unsigned char>(src[i])));
  dst[i] = '\0';
}

static bool containsSubstr(const char* haystack, const char* needle) {
  return strstr(haystack, needle) != nullptr;
}

// ---- lifecycle --------------------------------------------------------------

void SweepActivity::onEnter() {
  Activity::onEnter();
  state = READY;
  scanPhase = 0;
  findings.clear();
  findingIndex = 0;
  trackersFound = 0;
  suspiciousCams = 0;
  rogueAps = 0;
  skimmers = 0;
  requestUpdate();
}

void SweepActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

// ---- finding management -----------------------------------------------------

void SweepActivity::addFinding(const char* desc, int severity) {
  if (static_cast<int>(findings.size()) >= 50) return;
  Finding f;
  snprintf(f.description, sizeof(f.description), "%s", desc);
  f.severity = severity;
  findings.push_back(f);
}

// ---- sweep phases -----------------------------------------------------------

void SweepActivity::startSweep() {
  state = SCANNING;
  scanPhase = 0;
  findings.clear();
  findingIndex = 0;
  trackersFound = 0;
  suspiciousCams = 0;
  rogueAps = 0;
  skimmers = 0;
  requestUpdate();
}

void SweepActivity::scanWifiCameras() {
  RADIO.ensureWifi();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  int n = WiFi.scanNetworks();  // blocking scan
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String ssidRaw = WiFi.SSID(i);
      char upper[65];
      toUpperBuf(ssidRaw.c_str(), upper, sizeof(upper));

      for (int p = 0; p < CAMERA_PATTERN_COUNT; p++) {
        if (containsSubstr(upper, CAMERA_PATTERNS[p])) {
          char buf[80];
          snprintf(buf, sizeof(buf), "Camera: %.40s (ch %d)", ssidRaw.c_str(), WiFi.channel(i));
          addFinding(buf, 1);
          suspiciousCams++;
          break;
        }
      }
    }
  }
  WiFi.scanDelete();
}

void SweepActivity::scanWifiKarma() {
  // Scan with show_hidden=true; hidden APs (empty SSID) may be karma/rogue APs
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String ssidRaw = WiFi.SSID(i);
      if (ssidRaw.length() == 0) {
        uint8_t* bssid = WiFi.BSSID(i);
        char buf[80];
        if (bssid) {
          snprintf(buf, sizeof(buf), "Hidden AP: %02X:%02X:%02X:%02X:%02X:%02X ch%d %ddBm",
                   bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                   WiFi.channel(i), WiFi.RSSI(i));
        } else {
          snprintf(buf, sizeof(buf), "Hidden AP: unknown ch%d %ddBm",
                   WiFi.channel(i), WiFi.RSSI(i));
        }
        addFinding(buf, 1);
        rogueAps++;
      }
    }
  }
  WiFi.scanDelete();
}

void SweepActivity::scanBleThreats() {
  RADIO.ensureBle();  // shuts down WiFi first, then starts BLE

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->start(1);  // 1-second blocking scan (shorter to reduce UI freeze)

  BLEScanResults* results = scan->getResults();
  if (results) {
    int count = results->getCount();
    for (int i = 0; i < count; i++) {
      BLEAdvertisedDevice dev = results->getDevice(i);
      std::string mac = dev.getAddress().toString().c_str();
      int8_t rssi = static_cast<int8_t>(dev.getRSSI());

      bool isTracker = false;
      bool isSkimmer = false;
      const char* trackerLabel = nullptr;

      // --- Tracker detection ---
      if (dev.haveManufacturerData()) {
        String mfRaw = dev.getManufacturerData();
        const uint8_t* mf = reinterpret_cast<const uint8_t*>(mfRaw.c_str());
        size_t mfLen = static_cast<size_t>(mfRaw.length());

        if (mfLen >= 2) {
          uint16_t companyId = mf[0] | (static_cast<uint16_t>(mf[1]) << 8);

          // Apple Find My / AirTag: company 0x004C, length > 4
          if (companyId == 0x004C && mfLen > 4) {
            isTracker = true;
            trackerLabel = (mfLen >= 5 && mf[2] == 0x12 && mf[3] == 0x19)
                               ? "AirTag"
                               : "Apple FindMy";
          }

          // Samsung SmartTag: company 0x0075
          if (companyId == 0x0075) {
            isTracker = true;
            trackerLabel = "SmartTag";
          }
        }
      }

      // Tile: service UUID contains "feed"
      if (!isTracker && dev.haveServiceUUID()) {
        std::string svc = dev.getServiceUUID().toString().c_str();
        if (svc.find("feed") != std::string::npos || svc.find("FEED") != std::string::npos) {
          isTracker = true;
          trackerLabel = "Tile";
        }
      }

      // --- Skimmer OUI check ---
      // BLE address: parse first 3 bytes from "XX:XX:XX:XX:XX:XX"
      uint8_t addrBytes[6] = {};
      if (mac.length() >= 17) {
        unsigned int v[6];
        if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
          for (int b = 0; b < 6; b++) addrBytes[b] = static_cast<uint8_t>(v[b]);
        }
      }
      for (int o = 0; o < SKIMMER_OUI_COUNT; o++) {
        if (addrBytes[0] == SKIMMER_OUIS[o].b0 &&
            addrBytes[1] == SKIMMER_OUIS[o].b1 &&
            addrBytes[2] == SKIMMER_OUIS[o].b2) {
          isSkimmer = true;
          break;
        }
      }

      if (isTracker) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Tracker: %s [%s] %ddBm",
                 trackerLabel ? trackerLabel : "Unknown",
                 mac.c_str(), static_cast<int>(rssi));
        addFinding(buf, 2);
        trackersFound++;
      }

      if (isSkimmer) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Suspicious BLE: %s (skimmer OUI) %ddBm",
                 mac.c_str(), static_cast<int>(rssi));
        addFinding(buf, 2);
        skimmers++;
      }
    }
  }

  scan->clearResults();
  RADIO.shutdown();
}

// ---- loop -------------------------------------------------------------------

void SweepActivity::loop() {
  switch (state) {
    case READY:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startSweep();
      }
      break;

    case SCANNING:
      // Advance spinner while waiting between phases
      if (millis() - lastSpinnerUpdate >= 600) {
        lastSpinnerUpdate = millis();
        spinnerFrame = (spinnerFrame + 1) % 3;
        requestUpdate();
      }
      // Advance through scan phases one at a time, yielding back to the main loop
      // between each phase so the watchdog is fed and the display can update.
      switch (scanPhase) {
        case 0:
          scanWifiCameras();
          scanPhase = 1;
          requestUpdate();
          return;
        case 1:
          scanWifiKarma();
          scanPhase = 2;
          requestUpdate();
          return;
        case 2:
          scanBleThreats();
          scanPhase = 3;
          requestUpdate();
          return;
        case 3:
          state = RESULTS;
          findingIndex = 0;
          requestUpdate();
          return;
      }
      break;

    case RESULTS: {
      const int count = static_cast<int>(findings.size());
      buttonNavigator.onNext([this, count] {
        if (count > 0) {
          findingIndex = ButtonNavigator::nextIndex(findingIndex, count);
          requestUpdate();
        }
      });
      buttonNavigator.onPrevious([this, count] {
        if (count > 0) {
          findingIndex = ButtonNavigator::previousIndex(findingIndex, count);
          requestUpdate();
        }
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      break;
    }
  }
}

// ---- render -----------------------------------------------------------------

void SweepActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case READY:   renderReady();    break;
    case SCANNING: renderScanning(); break;
    case RESULTS: renderResults();  break;
  }
  renderer.displayBuffer();
}

void SweepActivity::renderReady() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Security Sweep");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 60, "Counter-Surveillance Sweep");
  renderer.drawCenteredText(SMALL_FONT_ID, centerY - 30, "WiFi cameras  |  Hidden APs");
  renderer.drawCenteredText(SMALL_FONT_ID, centerY - 10, "BLE trackers  |  Skimmers");
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 20, "~30 second scan");
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 50, "Press Confirm to start sweep.");

  const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SweepActivity::renderScanning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Scanning...");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;

  const char* phaseDesc = "";
  switch (scanPhase) {
    case 0: phaseDesc = "Phase 1/3: WiFi Cameras...";    break;
    case 1: phaseDesc = "Phase 2/3: Hidden AP Check..."; break;
    case 2: phaseDesc = "Phase 3/3: BLE Scan...";        break;
    default: phaseDesc = "Scanning...";                  break;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, phaseDesc, true, EpdFontFamily::BOLD);

  char progressBuf[48];
  snprintf(progressBuf, sizeof(progressBuf), "Phase %d of 3", scanPhase + 1);
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 20, progressBuf);

  GUI.drawSpinner(renderer, pageWidth / 2, centerY + 80, "PLEASE WAIT...", spinnerFrame);
}

void SweepActivity::renderResults() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int totalThreats = trackersFound + suspiciousCams + rogueAps + skimmers;
  char subtitle[48];
  snprintf(subtitle, sizeof(subtitle), "%d threat%s found",
           totalThreats, totalThreats == 1 ? "" : "s");

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Sweep Complete", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Summary line
  char summaryBuf[80];
  snprintf(summaryBuf, sizeof(summaryBuf), "Cams: %d | Hidden APs: %d | Trackers: %d | Skimmers: %d",
           suspiciousCams, rogueAps, trackersFound, skimmers);

  const int summaryH = renderer.getLineHeight(SMALL_FONT_ID) + 6;
  renderer.drawText(SMALL_FONT_ID, 8, contentTop + 4, summaryBuf);

  // Findings list
  const int listTop = contentTop + summaryH + 4;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (findings.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listH / 2, "No threats detected.");
  } else {
    const int count = static_cast<int>(findings.size());
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listH},
        count, findingIndex,
        [this](int i) -> std::string {
          return findings[i].description;
        },
        [this](int i) -> std::string {
          switch (findings[i].severity) {
            case 2: return "CRITICAL";
            case 1: return "WARNING";
            default: return "INFO";
          }
        });
  }

  const auto labels = mappedInput.mapLabels("Back", "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
