#include "PerimeterWatchActivity.h"

#include <HalStorage.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void PerimeterWatchActivity::macToStr(const uint8_t* mac, char* buf, size_t bufLen) {
  snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void PerimeterWatchActivity::onEnter() {
  Activity::onEnter();
  state = SETUP;
  baseline.clear();
  intrusions.clear();
  reportIndex = 0;
  watchStartMs = 0;
  lastScanMs = 0;
  requestUpdate();
}

void PerimeterWatchActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

void PerimeterWatchActivity::doBaselineScan() {
  baseline.clear();
  int n = WiFi.scanNetworks(false, true);  // blocking, include hidden
  if (n < 0) n = 0;
  for (int i = 0; i < n && static_cast<int>(baseline.size()) < MAX_BASELINE; i++) {
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) continue;
    Mac m;
    memcpy(m.bytes, bssid, 6);
    baseline.push_back(m);
  }
  WiFi.scanDelete();
}

bool PerimeterWatchActivity::macInBaseline(const uint8_t* mac) const {
  for (const auto& m : baseline) {
    if (memcmp(m.bytes, mac, 6) == 0) return true;
  }
  return false;
}

void PerimeterWatchActivity::doWatchScan() {
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) n = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* bssid = WiFi.BSSID(i);
    if (!bssid) continue;
    if (!macInBaseline(bssid) && static_cast<int>(intrusions.size()) < MAX_INTRUSIONS) {
      // Check if already logged
      bool dup = false;
      for (const auto& intr : intrusions) {
        if (memcmp(intr.mac, bssid, 6) == 0) { dup = true; break; }
      }
      if (!dup) {
        Intrusion intr;
        memcpy(intr.mac, bssid, 6);
        intr.rssi = WiFi.RSSI(i);
        intr.timestamp = millis();
        intrusions.push_back(intr);
      }
    }
  }
  WiFi.scanDelete();
  lastScanMs = millis();
  requestUpdate();
}

void PerimeterWatchActivity::exportCsv() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/logs");
  char path[48];
  snprintf(path, sizeof(path), "/biscuit/logs/perimeter_%lu.csv", millis());
  auto f = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  f.println("mac,rssi,timestamp_ms");
  char macBuf[20];
  for (const auto& intr : intrusions) {
    macToStr(intr.mac, macBuf, sizeof(macBuf));
    char line[64];
    snprintf(line, sizeof(line), "%s,%d,%lu", macBuf, intr.rssi, intr.timestamp);
    f.println(line);
  }
  f.close();
}

void PerimeterWatchActivity::loop() {
  if (state == SETUP) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      RADIO.ensureWifi();
      doBaselineScan();
      state = WATCHING;
      watchStartMs = millis();
      lastScanMs = millis();
      requestUpdate();
    }
    return;
  }

  if (state == WATCHING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = REPORT;
      reportIndex = 0;
      requestUpdate();
      return;
    }
    if (millis() - lastScanMs >= SCAN_INTERVAL_MS) {
      doWatchScan();
    }
    return;
  }

  // REPORT
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    exportCsv();
    requestUpdate();
    return;
  }
  const int count = static_cast<int>(intrusions.size());
  buttonNavigator.onNext([this, count] {
    reportIndex = ButtonNavigator::nextIndex(reportIndex, count > 0 ? count : 1);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    reportIndex = ButtonNavigator::previousIndex(reportIndex, count > 0 ? count : 1);
    requestUpdate();
  });
}

void PerimeterWatchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Perimeter Watch");

  const int midY = pageHeight / 2;
  char buf[64];

  if (state == SETUP) {
    snprintf(buf, sizeof(buf), "Baseline: %d device(s)", static_cast<int>(baseline.size()));
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 30, buf, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, "Confirm to start watching");
    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WATCHING) {
    unsigned long elapsed = (millis() - watchStartMs) / 1000UL;
    snprintf(buf, sizeof(buf), "Watching: %lus", elapsed);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 60, buf);
    snprintf(buf, sizeof(buf), "Baseline: %d", static_cast<int>(baseline.size()));
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, buf);
    snprintf(buf, sizeof(buf), "Intrusions: %d", static_cast<int>(intrusions.size()));
    renderer.drawCenteredText(UI_12_FONT_ID, midY + 20, buf, true, EpdFontFamily::BOLD);
    unsigned long scanElapsed = millis() - lastScanMs;
    unsigned long nextScan = (scanElapsed < SCAN_INTERVAL_MS) ? (SCAN_INTERVAL_MS - scanElapsed) / 1000UL : 0;
    snprintf(buf, sizeof(buf), "Next scan: %lus", nextScan);
    renderer.drawCenteredText(SMALL_FONT_ID, midY + 60, buf);
    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // REPORT
    const int count = static_cast<int>(intrusions.size());
    if (count == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY, "No intrusions detected");
    } else {
      int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      int contentH = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      char macBuf[20];
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentH}, count, reportIndex,
          [this, &macBuf](int i) -> std::string {
            macToStr(intrusions[i].mac, macBuf, sizeof(macBuf));
            return std::string(macBuf);
          },
          [this](int i) -> std::string {
            char sub[24];
            snprintf(sub, sizeof(sub), "%d dBm  +%lus", intrusions[i].rssi,
                     intrusions[i].timestamp / 1000UL);
            return std::string(sub);
          });
    }
    const auto labels = mappedInput.mapLabels("Back", "Export CSV", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
