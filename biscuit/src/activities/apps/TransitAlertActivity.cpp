#include "TransitAlertActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void TransitAlertActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  state = SETUP;
  setupMenuIndex = 0;
  stopIndex = 0;
  targetStop = 0;
  matchScore = 0.0f;
  lastScan = 0;
  loadStops();
  requestUpdate();
}

void TransitAlertActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

// ----------------------------------------------------------------
// Storage
// ----------------------------------------------------------------

void TransitAlertActivity::loadStops() {
  stops.clear();
  auto f = Storage.open(STOPS_PATH);
  if (!f) return;
  uint16_t count = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) { f.close(); return; }
  if (count == 0 || count > 32) { f.close(); return; }
  stops.resize(count);
  f.read(reinterpret_cast<uint8_t*>(stops.data()), sizeof(Stop) * count);
  f.close();
}

void TransitAlertActivity::saveStops() {
  Storage.mkdir("/biscuit");
  auto f = Storage.open(STOPS_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  uint16_t count = (uint16_t)stops.size();
  f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
  f.write(reinterpret_cast<const uint8_t*>(stops.data()), sizeof(Stop) * count);
  f.close();
}

// ----------------------------------------------------------------
// WiFi helpers
// ----------------------------------------------------------------

float TransitAlertActivity::calcMatch(int idx, const uint8_t bssids[][6], int count) const {
  if (idx < 0 || idx >= (int)stops.size()) return 0.0f;
  const Stop& s = stops[idx];
  if (s.apCount == 0) return 0.0f;
  int matched = 0;
  for (int i = 0; i < s.apCount; i++)
    for (int j = 0; j < count; j++)
      if (memcmp(s.bssids[i], bssids[j], 6) == 0) { matched++; break; }
  int unionCount = s.apCount + count - matched;
  if (unionCount <= 0) return 0.0f;
  return (float)matched / (float)unionCount * 100.0f;
}

void TransitAlertActivity::doScan() {
  static uint8_t scannedBssids[20][6];
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) { WiFi.scanDelete(); return; }
  int total = (n > 20) ? 20 : n;
  for (int i = 0; i < total; i++) memcpy(scannedBssids[i], WiFi.BSSID(i), 6);
  matchScore = calcMatch(targetStop, scannedBssids, total);
  WiFi.scanDelete();
}

// ----------------------------------------------------------------
// Save current stop (called from prompt callback)
// ----------------------------------------------------------------

void TransitAlertActivity::promptStopName() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Stop Name", "Stop", 31),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          if (!text.empty() && (int)stops.size() < 32) {
            // Scan WiFi and capture top 5 APs
            Stop s{};
            strncpy(s.name, text.c_str(), sizeof(s.name) - 1);
            int n = WiFi.scanNetworks(false, true);
            if (n > 0) {
              int total = (n > 5) ? 5 : n;
              // Sort by RSSI descending
              static int order[20];
              int scanned = (n > 20) ? 20 : n;
              for (int i = 0; i < scanned; i++) order[i] = i;
              for (int i = 0; i < scanned - 1; i++)
                for (int j = i + 1; j < scanned; j++)
                  if (WiFi.RSSI(order[j]) > WiFi.RSSI(order[i])) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
              total = (scanned < 5) ? scanned : 5;
              s.apCount = total;
              for (int i = 0; i < total; i++) {
                memcpy(s.bssids[i], WiFi.BSSID(order[i]), 6);
                s.rssis[i] = (int8_t)WiFi.RSSI(order[i]);
              }
              WiFi.scanDelete();
            }
            stops.push_back(s);
            saveStops();
          }
        }
        state = SETUP;
        setupMenuIndex = 0;
        requestUpdate();
      });
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void TransitAlertActivity::loop() {
  if (state == SETUP) {
    // Menu: [saved stops...] + "Save Current Stop" + "Start Monitoring"
    int menuCount = (int)stops.size() + 2;
    buttonNavigator.onNext([this, menuCount] { setupMenuIndex = ButtonNavigator::nextIndex(setupMenuIndex, menuCount); requestUpdate(); });
    buttonNavigator.onPrevious([this, menuCount] { setupMenuIndex = ButtonNavigator::previousIndex(setupMenuIndex, menuCount); requestUpdate(); });
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      int saveIdx = (int)stops.size();
      int startIdx = (int)stops.size() + 1;
      if (setupMenuIndex == saveIdx) {
        promptStopName();
      } else if (setupMenuIndex == startIdx) {
        if (!stops.empty()) {
          targetStop = 0;
          stopIndex = 0;
          state = SELECT_TARGET;
          requestUpdate();
        }
      }
      // Else: selected a stop for info (no action)
    }
    return;
  }

  if (state == SELECT_TARGET) {
    buttonNavigator.onNext([this] { targetStop = ButtonNavigator::nextIndex(targetStop, (int)stops.size()); requestUpdate(); });
    buttonNavigator.onPrevious([this] { targetStop = ButtonNavigator::previousIndex(targetStop, (int)stops.size()); requestUpdate(); });
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = SETUP; requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      matchScore = 0.0f;
      lastScan = 0;
      state = MONITORING;
      requestUpdate();
    }
    return;
  }

  if (state == MONITORING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = SETUP; setupMenuIndex = 0; requestUpdate(); return; }
    unsigned long now = millis();
    if (now - lastScan >= 15000) {
      lastScan = now;
      doScan();
      if (matchScore >= ALERT_THRESHOLD) {
        state = ALERT;
      }
      requestUpdate();
    }
    return;
  }

  if (state == ALERT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      matchScore = 0.0f;
      lastScan = 0;
      state = MONITORING;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = SETUP;
      setupMenuIndex = 0;
      requestUpdate();
    }
    return;
  }
}

// ----------------------------------------------------------------
// Render
// ----------------------------------------------------------------

void TransitAlertActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case SETUP:         renderSetup();         break;
    case SELECT_TARGET: renderSelectTarget();  break;
    case MONITORING:    renderMonitoring();    break;
    case ALERT:         renderAlert();         break;
  }
  renderer.displayBuffer();
}

void TransitAlertActivity::renderSetup() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Transit Alert");
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  int menuCount = (int)stops.size() + 2;
  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, menuCount, setupMenuIndex,
               [this](int i) -> std::string {
                 if (i < (int)stops.size()) return stops[i].name;
                 if (i == (int)stops.size()) return "Save Current Stop";
                 return "Start Monitoring";
               });
  const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TransitAlertActivity::renderSelectTarget() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Select Target Stop");
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, (int)stops.size(), targetStop,
               [this](int i) -> std::string {
                 char buf[48];
                 snprintf(buf, sizeof(buf), "%s (%d APs)", stops[i].name, stops[i].apCount);
                 return buf;
               });
  const auto labels = mappedInput.mapLabels("Back", "Monitor", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TransitAlertActivity::renderMonitoring() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Monitoring");

  int y = metrics.topPadding + metrics.headerHeight + 30;
  if (targetStop < (int)stops.size()) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Target: %s", stops[targetStop].name);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    y += 40;
  }

  char pctBuf[24];
  snprintf(pctBuf, sizeof(pctBuf), "Match: %.0f%%", matchScore);
  renderer.drawCenteredText(UI_12_FONT_ID, y, pctBuf, true, EpdFontFamily::BOLD);
  y += 55;

  unsigned long nextIn = (lastScan > 0 && (millis() - lastScan) < 15000) ? (15 - (millis() - lastScan) / 1000) : 0;
  char buf[28];
  snprintf(buf, sizeof(buf), "Next scan: %lus", nextIn);
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  (void)pageHeight;
}

void TransitAlertActivity::renderAlert() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Bold border to draw attention
  renderer.drawRect(4, 4, pageWidth - 8, pageHeight - 8, true);
  renderer.drawRect(8, 8, pageWidth - 16, pageHeight - 16, true);

  int y = pageHeight / 2 - 60;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "APPROACHING:", true, EpdFontFamily::BOLD);
  y += 55;
  if (targetStop < (int)stops.size()) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, stops[targetStop].name, true, EpdFontFamily::BOLD);
    y += 55;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "Match: %.0f%%", matchScore);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);

  const auto labels = mappedInput.mapLabels("Stop", "Dismiss", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
