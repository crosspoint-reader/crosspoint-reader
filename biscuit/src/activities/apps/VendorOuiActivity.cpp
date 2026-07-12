#include "VendorOuiActivity.h"

#include <HalStorage.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

static const char* const MENU_LABELS[] = {"Enter MAC manually", "Pick from WiFi scan"};
static constexpr int MENU_COUNT = 2;
static constexpr const char* OUI_PATH = "/biscuit/oui.txt";

void VendorOuiActivity::formatMac(const uint8_t* bytes, char* buf, size_t len) {
  snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

bool VendorOuiActivity::parseHexMac(const char* str, uint8_t* out6) {
  // Accept: AABBCCDDEEFF or AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF
  char clean[13];
  int ci = 0;
  for (int i = 0; str[i] && ci < 12; i++) {
    char c = str[i];
    if (c == ':' || c == '-') continue;
    clean[ci++] = c;
  }
  clean[ci] = '\0';
  if (ci < 6) return false;  // need at least OUI (3 bytes = 6 hex chars)
  for (int b = 0; b < 6; b++) {
    char hex[3] = {clean[b * 2], clean[b * 2 + 1], '\0'};
    out6[b] = static_cast<uint8_t>(strtol(hex, nullptr, 16));
  }
  return true;
}

void VendorOuiActivity::lookupOui(const uint8_t* ouiBytes) {
  vendorResult = "Unknown vendor";
  auto f = Storage.open(OUI_PATH);
  if (!f) { vendorResult = "oui.txt not found"; return; }

  // Build 6-char hex OUI prefix to match
  char prefix[7];
  snprintf(prefix, sizeof(prefix), "%02X%02X%02X", ouiBytes[0], ouiBytes[1], ouiBytes[2]);

  char line[128];
  int idx = 0;
  bool found = false;
  while (!found) {
    int c = f.read();
    if (c < 0) break;
    if (static_cast<char>(c) == '\n' || idx >= static_cast<int>(sizeof(line)) - 1) {
      line[idx] = '\0';
      // Line format: AABBCC\tVendor Name
      if (idx >= 6) {
        // Case-insensitive compare first 6 chars
        bool match = true;
        for (int i = 0; i < 6; i++) {
          char a = line[i];
          char b = prefix[i];
          if (a >= 'a' && a <= 'f') a -= 32;
          if (b >= 'a' && b <= 'f') b -= 32;
          if (a != b) { match = false; break; }
        }
        if (match && line[6] == '\t') {
          vendorResult = std::string(line + 7);
          found = true;
        }
      }
      idx = 0;
    } else {
      line[idx++] = static_cast<char>(c);
    }
  }
  f.close();
}

void VendorOuiActivity::onEnter() {
  Activity::onEnter();
  state = MENU;
  menuIndex = 0;
  scanIndex = 0;
  inputMac.clear();
  displayMac.clear();
  vendorResult.clear();
  lookupDone = false;
  scanResults.clear();
  requestUpdate();
}

void VendorOuiActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

void VendorOuiActivity::startKeyboardEntry() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Enter MAC Address", "", 17),
      [this](const ActivityResult& result) {
        if (result.isCancelled) { state = MENU; return; }
        inputMac = std::get<KeyboardResult>(result.data).text;
        uint8_t mac[6] = {};
        if (!parseHexMac(inputMac.c_str(), mac)) {
          vendorResult = "Invalid MAC format";
          char macBuf[20] = "??:??:??:??:??:??";
          displayMac = macBuf;
        } else {
          char macBuf[20];
          formatMac(mac, macBuf, sizeof(macBuf));
          displayMac = macBuf;
          lookupOui(mac);
        }
        lookupDone = true;
        state = RESULT;
      });
}

void VendorOuiActivity::startWifiScan() {
  RADIO.ensureWifi();
  state = SCANNING;
  scanResults.clear();
  requestUpdate();
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) n = 0;
  for (int i = 0; i < n && i < 40; i++) {
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) continue;
    ScannedAp ap;
    memcpy(ap.bssid, bssid, 6);
    ap.rssi = WiFi.RSSI(i);
    ap.ssid = WiFi.SSID(i).c_str();
    scanResults.push_back(ap);
  }
  WiFi.scanDelete();
  scanIndex = 0;
  state = SCAN_LIST;
  requestUpdate();
}

void VendorOuiActivity::loop() {
  if (state == MENU) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    buttonNavigator.onNext([this] {
      menuIndex = ButtonNavigator::nextIndex(menuIndex, MENU_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      menuIndex = ButtonNavigator::previousIndex(menuIndex, MENU_COUNT);
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (menuIndex == 0) startKeyboardEntry();
      else startWifiScan();
    }
    return;
  }

  if (state == SCANNING) {
    // blocking scan — no input handling needed, just wait
    return;
  }

  if (state == SCAN_LIST) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = MENU; requestUpdate(); return; }
    const int count = static_cast<int>(scanResults.size());
    buttonNavigator.onNext([this, count] {
      if (count > 0) { scanIndex = ButtonNavigator::nextIndex(scanIndex, count); requestUpdate(); }
    });
    buttonNavigator.onPrevious([this, count] {
      if (count > 0) { scanIndex = ButtonNavigator::previousIndex(scanIndex, count); requestUpdate(); }
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!scanResults.empty()) {
        const auto& ap = scanResults[scanIndex];
        char macBuf[20];
        formatMac(ap.bssid, macBuf, sizeof(macBuf));
        displayMac = macBuf;
        lookupOui(ap.bssid);
        lookupDone = true;
        state = RESULT;
        requestUpdate();
      }
    }
    return;
  }

  // RESULT
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state = MENU;
    lookupDone = false;
    requestUpdate();
  }
}

void VendorOuiActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Vendor OUI Lookup");

  const int midY = pageHeight / 2;

  if (state == MENU) {
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    int contentH = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentH}, MENU_COUNT, menuIndex,
        [](int i) -> std::string { return MENU_LABELS[i]; });
    const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SCANNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, "Scanning WiFi...");
  } else if (state == SCAN_LIST) {
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    int contentH = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int count = static_cast<int>(scanResults.size());
    if (count == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY, "No APs found");
    } else {
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentH}, count, scanIndex,
          [this](int i) -> std::string { return scanResults[i].ssid.empty() ? "(hidden)" : scanResults[i].ssid; },
          [this](int i) -> std::string {
            char buf[20];
            formatMac(scanResults[i].bssid, buf, sizeof(buf));
            return std::string(buf);
          });
    }
    const auto labels = mappedInput.mapLabels("Back", "Lookup", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // RESULT
    int y = metrics.topPadding + metrics.headerHeight + 40;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "MAC Address");
    y += 25;
    renderer.drawCenteredText(UI_10_FONT_ID, y, displayMac.c_str(), true, EpdFontFamily::BOLD);
    y += 50;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "Vendor");
    y += 25;
    renderer.drawCenteredText(UI_12_FONT_ID, y, vendorResult.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
