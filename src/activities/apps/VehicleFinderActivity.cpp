#include "VehicleFinderActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void VehicleFinderActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  state = IDLE;
  currentMatch = 0.0f;
  prevMatch = 0.0f;
  lastScan = 0;
  trendHead = 0;
  trendCount = 0;
  loadSpot();
  requestUpdate();
}

void VehicleFinderActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

// ----------------------------------------------------------------
// Storage
// ----------------------------------------------------------------

void VehicleFinderActivity::loadSpot() {
  savedApCount = 0;
  hasSavedSpot = false;
  auto f = Storage.open(SAVE_PATH);
  if (!f) return;
  uint8_t count = 0;
  if (f.read(&count, 1) != 1 || count == 0 || count > 10) { f.close(); return; }
  savedApCount = count;
  f.read(reinterpret_cast<uint8_t*>(savedAps), sizeof(Fingerprint) * savedApCount);
  f.close();
  hasSavedSpot = true;
}

void VehicleFinderActivity::saveSpot() {
  Storage.mkdir("/biscuit");
  auto f = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  uint8_t count = (uint8_t)savedApCount;
  f.write(&count, 1);
  f.write(reinterpret_cast<const uint8_t*>(savedAps), sizeof(Fingerprint) * savedApCount);
  f.close();
  hasSavedSpot = true;
}

// ----------------------------------------------------------------
// WiFi helpers
// ----------------------------------------------------------------

float VehicleFinderActivity::calcMatch(const Fingerprint* current, int count) const {
  if (savedApCount == 0) return 0.0f;
  int matched = 0;
  float rssiBonus = 0.0f;
  for (int i = 0; i < savedApCount; i++) {
    for (int j = 0; j < count; j++) {
      if (memcmp(savedAps[i].bssid, current[j].bssid, 6) == 0) {
        matched++;
        // Bonus: closer RSSI = higher bonus, max 0.5 per AP
        float diff = fabsf((float)(savedAps[i].rssi - current[j].rssi));
        rssiBonus += (diff < 20.0f) ? (1.0f - diff / 20.0f) * 0.5f : 0.0f;
        break;
      }
    }
  }
  float base = (float)matched / (float)savedApCount * 100.0f;
  float bonus = rssiBonus / (float)savedApCount * 10.0f;  // up to 5% bonus
  float score = base + bonus;
  if (score > 100.0f) score = 100.0f;
  return score;
}

void VehicleFinderActivity::doScan() {
  static Fingerprint buf[20];
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) { WiFi.scanDelete(); return; }
  int total = (n > 20) ? 20 : n;

  // Sort descending by RSSI
  for (int i = 0; i < total; i++) {
    memcpy(buf[i].bssid, WiFi.BSSID(i), 6);
    buf[i].rssi = (int8_t)WiFi.RSSI(i);
  }
  for (int i = 0; i < total - 1; i++)
    for (int j = i + 1; j < total; j++)
      if (buf[j].rssi > buf[i].rssi) {
        Fingerprint tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
      }

  prevMatch = currentMatch;
  currentMatch = calcMatch(buf, total);

  // Trend: +1 up, -1 down, 0 stable
  int trend = 0;
  if (currentMatch > prevMatch + 1.0f) trend = 1;
  else if (currentMatch < prevMatch - 1.0f) trend = -1;
  trendHistory[trendHead] = trend;
  trendHead = (trendHead + 1) % 5;
  if (trendCount < 5) trendCount++;

  WiFi.scanDelete();
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void VehicleFinderActivity::loop() {
  if (state == IDLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // First option: Save Spot
      int n = WiFi.scanNetworks(false, true);
      if (n > 0) {
        savedApCount = (n > 10) ? 10 : n;
        for (int i = 0; i < savedApCount; i++) {
          memcpy(savedAps[i].bssid, WiFi.BSSID(i), 6);
          savedAps[i].rssi = (int8_t)WiFi.RSSI(i);
        }
        WiFi.scanDelete();
        saveSpot();
      }
      state = SAVED;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) && hasSavedSpot) {
      // Second option: Find Car
      currentMatch = 0.0f;
      prevMatch = 0.0f;
      lastScan = 0;
      trendHead = 0;
      trendCount = 0;
      state = FINDING;
      requestUpdate();
    }
    return;
  }

  if (state == SAVED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = IDLE; requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      currentMatch = 0.0f;
      prevMatch = 0.0f;
      lastScan = 0;
      trendHead = 0;
      trendCount = 0;
      state = FINDING;
      requestUpdate();
    }
    return;
  }

  if (state == FINDING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = IDLE; requestUpdate(); return; }
    unsigned long now = millis();
    if (now - lastScan >= 3000) {
      lastScan = now;
      doScan();
      requestUpdate();
    }
    return;
  }
}

// ----------------------------------------------------------------
// Render
// ----------------------------------------------------------------

void VehicleFinderActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case IDLE:    renderIdle();    break;
    case SAVED:   renderSaved();   break;
    case FINDING: renderFinding(); break;
  }
  renderer.displayBuffer();
}

void VehicleFinderActivity::renderIdle() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Vehicle Finder");

  int y = metrics.topPadding + metrics.headerHeight + 40;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Scan and save your current parking spot,");
  y += 30;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "then navigate back using WiFi signals.");
  y += 50;

  if (hasSavedSpot) {
    char buf[40];
    snprintf(buf, sizeof(buf), "Saved spot: %d APs", savedApCount);
    renderer.drawCenteredText(SMALL_FONT_ID, y, buf);
    y += 25;
    renderer.drawCenteredText(SMALL_FONT_ID, y, "Right = Find Car");
  }

  const auto labels = mappedInput.mapLabels("Back", "Save Spot", "", hasSavedSpot ? "Find Car" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void VehicleFinderActivity::renderSaved() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Spot Saved");

  int y = metrics.topPadding + metrics.headerHeight + 60;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Spot Saved!", true, EpdFontFamily::BOLD);
  y += 50;
  char buf[40];
  snprintf(buf, sizeof(buf), "%d APs recorded.", savedApCount);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 35;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Confirm to start finding.");

  const auto labels = mappedInput.mapLabels("Back", "Find Car", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void VehicleFinderActivity::renderFinding() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Finding Car");

  int y = metrics.topPadding + metrics.headerHeight + 20;

  // Large match percentage
  char pctBuf[16];
  snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", currentMatch);
  renderer.drawCenteredText(UI_12_FONT_ID, y, pctBuf, true, EpdFontFamily::BOLD);
  y += 60;

  // Direction indicator
  const char* direction = "STABLE";
  if (currentMatch > prevMatch + 1.0f) direction = "GETTING CLOSER";
  else if (currentMatch < prevMatch - 1.0f) direction = "WRONG WAY";
  renderer.drawCenteredText(UI_10_FONT_ID, y, direction);
  y += 45;

  // Trend history bar (last 5 readings)
  if (trendCount > 0) {
    char trendBuf[16] = {};
    int writeIdx = (trendHead - trendCount + 5) % 5;
    for (int i = 0; i < trendCount; i++) {
      int val = trendHistory[(writeIdx + i) % 5];
      trendBuf[i] = (val > 0) ? '^' : (val < 0) ? 'v' : '-';
    }
    trendBuf[trendCount] = '\0';
    renderer.drawCenteredText(SMALL_FONT_ID, y, trendBuf);
    y += 25;
  }

  unsigned long nextIn = (lastScan > 0 && (millis() - lastScan) < 3000) ? (3 - (millis() - lastScan) / 1000) : 0;
  char buf[24];
  snprintf(buf, sizeof(buf), "Scan in: %lus", nextIn);
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  (void)pageHeight;
}
