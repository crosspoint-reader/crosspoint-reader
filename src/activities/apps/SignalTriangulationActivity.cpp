#include "SignalTriangulationActivity.h"

#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---- onEnter / onExit -------------------------------------------------------

void SignalTriangulationActivity::onEnter() {
  Activity::onEnter();
  state = SELECT_AP;
  apCount = 0;
  apIndex = 0;
  initialScanDone = false;
  initialScanning = false;
  scanning = false;
  currentReading = 0;
  for (int i = 0; i < 3; i++) {
    readings[i].rssi = 0;
    readings[i].taken = false;
  }
  RADIO.ensureWifi();
  WiFi.disconnect();
  startInitialScan();
  requestUpdate();
}

void SignalTriangulationActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

// ---- Scan helpers -----------------------------------------------------------

void SignalTriangulationActivity::startInitialScan() {
  initialScanning = true;
  initialScanDone = false;
  apCount = 0;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
}

void SignalTriangulationActivity::processInitialScan() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  apCount = 0;
  if (result > 0) {
    for (int i = 0; i < result && apCount < MAX_APS; i++) {
      VisibleAp& ap = apList[apCount];

      String ssid = WiFi.SSID(i);
      int ssidLen = ssid.length();
      if (ssidLen >= static_cast<int>(sizeof(ap.ssid))) ssidLen = sizeof(ap.ssid) - 1;
      memcpy(ap.ssid, ssid.c_str(), ssidLen);
      ap.ssid[ssidLen] = '\0';
      if (ap.ssid[0] == '\0') {
        memcpy(ap.ssid, "(hidden)", 9);
      }

      uint8_t* bssidBytes = WiFi.BSSID(i);
      if (bssidBytes) {
        snprintf(ap.bssid, sizeof(ap.bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bssidBytes[0], bssidBytes[1], bssidBytes[2],
                 bssidBytes[3], bssidBytes[4], bssidBytes[5]);
      } else {
        ap.bssid[0] = '\0';
      }

      ap.rssi = static_cast<int8_t>(WiFi.RSSI(i));
      ap.channel = static_cast<uint8_t>(WiFi.channel(i));
      apCount++;
    }
  }
  WiFi.scanDelete();
  initialScanning = false;
  initialScanDone = true;
  requestUpdate();
}

void SignalTriangulationActivity::startReadingScan() {
  scanning = true;
  scanSamples = 0;
  rssiAccumulator = 0;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
}

void SignalTriangulationActivity::processReadingScan() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  if (result > 0) {
    for (int i = 0; i < result; i++) {
      uint8_t* bssidBytes = WiFi.BSSID(i);
      if (!bssidBytes) continue;
      char bssidStr[18];
      snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               bssidBytes[0], bssidBytes[1], bssidBytes[2],
               bssidBytes[3], bssidBytes[4], bssidBytes[5]);
      if (strcmp(bssidStr, targetBssid) == 0) {
        rssiAccumulator += WiFi.RSSI(i);
        scanSamples++;
        break;
      }
    }
  }
  WiFi.scanDelete();

  if (scanSamples >= SAMPLES_PER_READING) {
    readings[currentReading].rssi = static_cast<int8_t>(rssiAccumulator / scanSamples);
    readings[currentReading].taken = true;
    scanning = false;
    currentReading++;
    if (currentReading >= 3) {
      state = RESULT;
    }
    requestUpdate();
  } else {
    // Need more samples — kick off another scan
    WiFi.scanNetworks(true);
  }
}

// ---- loop -------------------------------------------------------------------

void SignalTriangulationActivity::loop() {
  // Always handle Back
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == TAKE_READINGS || state == RESULT) {
      // Return to AP selection
      state = SELECT_AP;
      scanning = false;
      WiFi.scanDelete();
      currentReading = 0;
      for (int i = 0; i < 3; i++) {
        readings[i].taken = false;
        readings[i].rssi = 0;
      }
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state == SELECT_AP) {
    if (initialScanning) {
      processInitialScan();
      return;
    }

    const int count = apCount;

    buttonNavigator.onNext([this, count] {
      if (count > 0) {
        apIndex = ButtonNavigator::nextIndex(apIndex, count);
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this, count] {
      if (count > 0) {
        apIndex = ButtonNavigator::previousIndex(apIndex, count);
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (apCount > 0) {
        const VisibleAp& selected = apList[apIndex];
        memcpy(targetBssid, selected.bssid, sizeof(targetBssid));
        memcpy(targetSsid, selected.ssid, sizeof(targetSsid));
        state = TAKE_READINGS;
        currentReading = 0;
        for (int i = 0; i < 3; i++) {
          readings[i].taken = false;
          readings[i].rssi = 0;
        }
        scanning = false;
        requestUpdate();
      }
    }

    // PageForward: rescan
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      startInitialScan();
      requestUpdate();
    }
    return;
  }

  if (state == TAKE_READINGS) {
    if (scanning) {
      processReadingScan();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (currentReading < 3 && !scanning) {
        startReadingScan();
        requestUpdate();
      }
    }
    return;
  }

  // RESULT state — Back already handled above, nothing else to do
}

// ---- render -----------------------------------------------------------------

void SignalTriangulationActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Triangulation");

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int pad = metrics.contentSidePadding;

  // ---- SELECT_AP ----
  if (state == SELECT_AP) {
    if (initialScanning) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Scanning for APs...");
      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    if (apCount == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No APs found");
      const auto labels = mappedInput.mapLabels("Back", "", "", "Rescan");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    const int contentTop = headerBottom + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, apCount, apIndex,
        [this](int i) -> std::string { return apList[i].ssid; },
        [this](int i) -> std::string {
          char buf[48];
          snprintf(buf, sizeof(buf), "%d dBm  Ch%d  %s",
                   static_cast<int>(apList[i].rssi),
                   static_cast<int>(apList[i].channel),
                   apList[i].bssid);
          return buf;
        });

    const auto labels = mappedInput.mapLabels("Back", "Select", "", "Rescan");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ---- TAKE_READINGS ----
  if (state == TAKE_READINGS) {
    // Subtitle: target AP
    char subtitle[52];
    snprintf(subtitle, sizeof(subtitle), "Target: %.33s", targetSsid);
    renderer.drawText(SMALL_FONT_ID, pad, headerBottom + 8, subtitle);

    int y = headerBottom + 32;

    // Instruction line
    if (scanning) {
      char scanMsg[32];
      snprintf(scanMsg, sizeof(scanMsg), "Scanning... (%d/%d)", scanSamples, SAMPLES_PER_READING);
      renderer.drawCenteredText(UI_10_FONT_ID, y, scanMsg);
    } else if (currentReading < 3) {
      char instrBuf[48];
      snprintf(instrBuf, sizeof(instrBuf), "Move to position %d, press OK", currentReading + 1);
      renderer.drawCenteredText(UI_10_FONT_ID, y, instrBuf);
    }
    y += renderer.getTextHeight(UI_10_FONT_ID) + 16;

    // Show readings taken so far
    for (int i = 0; i < 3; i++) {
      char lineBuf[40];
      if (readings[i].taken) {
        snprintf(lineBuf, sizeof(lineBuf), "Point %d: %d dBm", i + 1, static_cast<int>(readings[i].rssi));
      } else if (i == currentReading && scanning) {
        snprintf(lineBuf, sizeof(lineBuf), "Point %d: measuring...", i + 1);
      } else {
        snprintf(lineBuf, sizeof(lineBuf), "Point %d: (pending)", i + 1);
      }
      renderer.drawText(UI_10_FONT_ID, pad, y, lineBuf);
      y += renderer.getTextHeight(UI_10_FONT_ID) + 8;
    }

    if (!scanning && currentReading < 3) {
      const auto labels = mappedInput.mapLabels("Back", "Take Reading", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const auto labels = mappedInput.mapLabels("Back", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  // ---- RESULT ----
  // Find strongest and weakest readings
  int strongestIdx = 0;
  int weakestIdx = 0;
  for (int i = 1; i < 3; i++) {
    if (readings[i].rssi > readings[strongestIdx].rssi) strongestIdx = i;
    if (readings[i].rssi < readings[weakestIdx].rssi) weakestIdx = i;
  }

  // Draw the 3-point visual diagram
  // Layout: Point 1 top-center, Point 2 bottom-left, Point 3 bottom-right
  static constexpr int CIRCLE_R = 18;
  static constexpr int CIRCLE_D = CIRCLE_R * 2;

  const int diagCenterX = pageWidth / 2;
  const int diagTop = headerBottom + 20;

  // Positions for the three circles
  struct Pos { int x, y; };
  const Pos pts[3] = {
    { diagCenterX,          diagTop + CIRCLE_R },           // Point 1: top center
    { diagCenterX - 70,     diagTop + 90 + CIRCLE_R },      // Point 2: bottom left
    { diagCenterX + 70,     diagTop + 90 + CIRCLE_R },      // Point 3: bottom right
  };

  // Draw connecting lines between points
  renderer.drawLine(pts[0].x, pts[0].y, pts[1].x, pts[1].y, true);
  renderer.drawLine(pts[0].x, pts[0].y, pts[2].x, pts[2].y, true);
  renderer.drawLine(pts[1].x, pts[1].y, pts[2].x, pts[2].y, true);

  // Draw circles (filled for strongest)
  for (int i = 0; i < 3; i++) {
    const int cx = pts[i].x;
    const int cy = pts[i].y;
    if (i == strongestIdx) {
      // Filled rect approximating the circle for the strongest point
      renderer.fillRect(cx - CIRCLE_R, cy - CIRCLE_R, CIRCLE_D, CIRCLE_D, true);
      // Draw label in white (inverted) — just draw number on top
      char numBuf[3];
      snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
      int tw = renderer.getTextWidth(SMALL_FONT_ID, numBuf);
      int th = renderer.getTextHeight(SMALL_FONT_ID);
      // XOR trick: draw text in black on the filled rect
      renderer.drawText(SMALL_FONT_ID, cx - tw / 2, cy - th / 2, numBuf, false);
    } else {
      renderer.drawRect(cx - CIRCLE_R, cy - CIRCLE_R, CIRCLE_D, CIRCLE_D, true);
      char numBuf[3];
      snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
      int tw = renderer.getTextWidth(SMALL_FONT_ID, numBuf);
      int th = renderer.getTextHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, cx - tw / 2, cy - th / 2, numBuf, true);
    }
  }

  // Readings table
  int y = diagTop + 90 + CIRCLE_D + 20;

  for (int i = 0; i < 3; i++) {
    char lineBuf[48];
    if (i == strongestIdx) {
      snprintf(lineBuf, sizeof(lineBuf), "Point %d: %d dBm  << STRONGEST", i + 1, static_cast<int>(readings[i].rssi));
    } else if (i == weakestIdx) {
      snprintf(lineBuf, sizeof(lineBuf), "Point %d: %d dBm  (weakest)", i + 1, static_cast<int>(readings[i].rssi));
    } else {
      snprintf(lineBuf, sizeof(lineBuf), "Point %d: %d dBm", i + 1, static_cast<int>(readings[i].rssi));
    }
    const bool bold = (i == strongestIdx);
    renderer.drawText(UI_10_FONT_ID, pad, y, lineBuf, true, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += renderer.getTextHeight(UI_10_FONT_ID) + 6;
  }

  y += 8;
  char dirBuf[40];
  snprintf(dirBuf, sizeof(dirBuf), "Direction: toward Point %d", strongestIdx + 1);
  renderer.drawCenteredText(UI_10_FONT_ID, y, dirBuf, true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
