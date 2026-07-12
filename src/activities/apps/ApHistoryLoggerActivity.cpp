#include "ApHistoryLoggerActivity.h"

#include <HalStorage.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

constexpr unsigned long ApHistoryLoggerActivity::INTERVALS[];
constexpr const char* ApHistoryLoggerActivity::INTERVAL_LABELS[];

uint64_t ApHistoryLoggerActivity::bssidToU64(const uint8_t* bssid) {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | bssid[i];
  return v;
}

void ApHistoryLoggerActivity::onEnter() {
  Activity::onEnter();
  state = CONFIG;
  intervalIndex = 0;
  scanCount = 0;
  fileSize = 0;
  logStartMs = 0;
  lastScanMs = 0;
  seenBssids.clear();
  requestUpdate();
}

void ApHistoryLoggerActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

void ApHistoryLoggerActivity::doScan() {
  RADIO.ensureWifi();
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) n = 0;

  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");
  auto f = Storage.open(LOG_PATH, O_WRITE | O_CREAT | O_APPEND);

  unsigned long ts = millis();
  char line[128];

  for (int i = 0; i < n; i++) {
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) continue;

    // Track unique BSSIDs
    uint64_t id = bssidToU64(bssid);
    bool seen = false;
    for (uint64_t v : seenBssids) { if (v == id) { seen = true; break; } }
    if (!seen && static_cast<int>(seenBssids.size()) < MAX_SEEN) {
      seenBssids.push_back(id);
    }

    if (f) {
      String ssid = WiFi.SSID(i);
      // Escape commas in SSID
      std::string ssidStr = ssid.c_str();
      for (auto& c : ssidStr) { if (c == ',') c = ';'; }

      snprintf(line, sizeof(line), "%lu,%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%d",
               ts, ssidStr.c_str(),
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
               static_cast<int>(WiFi.RSSI(i)),
               static_cast<int>(WiFi.channel(i)));
      f.println(line);
    }
  }

  if (f) {
    fileSize = static_cast<uint32_t>(f.size());
    f.close();
  }

  WiFi.scanDelete();
  scanCount++;
  lastScanMs = millis();
  requestUpdate();
}

void ApHistoryLoggerActivity::loop() {
  if (state == CONFIG) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    buttonNavigator.onNext([this] {
      intervalIndex = ButtonNavigator::nextIndex(intervalIndex, NUM_INTERVALS);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      intervalIndex = ButtonNavigator::previousIndex(intervalIndex, NUM_INTERVALS);
      requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LOGGING;
      logStartMs = millis();
      lastScanMs = 0;  // force immediate first scan
      scanCount = 0;
      fileSize = 0;
      seenBssids.clear();
      requestUpdate();
    }
    return;
  }

  if (state == LOGGING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = STATS;
      requestUpdate();
      return;
    }
    unsigned long interval = INTERVALS[intervalIndex];
    if (lastScanMs == 0 || millis() - lastScanMs >= interval) {
      doScan();
    }
    return;
  }

  // STATS
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Restart
    state = CONFIG;
    requestUpdate();
  }
}

void ApHistoryLoggerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "AP History Logger");

  const int midY = pageHeight / 2;
  char buf[80];

  if (state == CONFIG) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 60, "Scan interval:");
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, INTERVAL_LABELS[intervalIndex], true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 20, "Logs to /biscuit/logs/ap_history.csv");
    const auto labels = mappedInput.mapLabels("Back", "Start", "Prev", "Next");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == LOGGING) {
    unsigned long elapsed = (millis() - logStartMs) / 1000UL;
    snprintf(buf, sizeof(buf), "Running: %lus", elapsed);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 80, buf);

    snprintf(buf, sizeof(buf), "Interval: %s", INTERVAL_LABELS[intervalIndex]);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 45, buf);

    snprintf(buf, sizeof(buf), "Scans: %d", scanCount);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, buf);

    snprintf(buf, sizeof(buf), "Unique APs: %d", static_cast<int>(seenBssids.size()));
    renderer.drawCenteredText(UI_12_FONT_ID, midY + 30, buf, true, EpdFontFamily::BOLD);

    snprintf(buf, sizeof(buf), "File: %lu B", static_cast<unsigned long>(fileSize));
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 70, buf);

    unsigned long nextIn = 0;
    if (lastScanMs > 0) {
      unsigned long elapsed2 = millis() - lastScanMs;
      unsigned long interval = INTERVALS[intervalIndex];
      nextIn = elapsed2 < interval ? (interval - elapsed2) / 1000UL : 0;
    }
    snprintf(buf, sizeof(buf), "Next scan: %lus", nextIn);
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 95, buf);

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // STATS
    unsigned long dur = logStartMs > 0 ? (millis() - logStartMs) / 1000UL : 0;
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 80, "Session complete");

    snprintf(buf, sizeof(buf), "Duration: %lus", dur);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, buf);

    snprintf(buf, sizeof(buf), "Total scans: %d", scanCount);
    renderer.drawCenteredText(UI_10_FONT_ID, midY, buf);

    snprintf(buf, sizeof(buf), "Unique APs: %d", static_cast<int>(seenBssids.size()));
    renderer.drawCenteredText(UI_12_FONT_ID, midY + 40, buf, true, EpdFontFamily::BOLD);

    snprintf(buf, sizeof(buf), "File size: %lu B", static_cast<unsigned long>(fileSize));
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 80, buf);

    const auto labels = mappedInput.mapLabels("Back", "New session", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
