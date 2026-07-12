#include "WifiHeatMapActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void WifiHeatMapActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  WiFi.disconnect();
  state = IDLE;
  currentCount = 0;
  seenCount = 0;
  totalDataPoints = 0;
  sampleCount = 0;
  selectorIndex = 0;
  filename[0] = '\0';
  headerWriteFailed = false;
  maxPointsReached = false;
  requestUpdate();
}

void WifiHeatMapActivity::onExit() {
  Activity::onExit();
  stopLogging();
  RADIO.shutdown();
}

bool WifiHeatMapActivity::isBssidSeen(const char* bssid) const {
  for (int i = 0; i < seenCount; i++) {
    if (strncmp(seenBssids[i], bssid, 17) == 0) return true;
  }
  return false;
}

void WifiHeatMapActivity::startLogging() {
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/heatmaps");

  snprintf(filename, sizeof(filename), "/biscuit/heatmaps/heatmap_%lu.csv", millis());

  // HEATMAP-003: check that header write succeeded before entering LOGGING state
  if (!Storage.writeFile(filename, "Sample,Timestamp_ms,BSSID,SSID,RSSI,Channel\n")) {
    LOG_DBG("HMAP", "Failed to write CSV header to %s", filename);
    headerWriteFailed = true;
    requestUpdate();
    return;
  }

  startTime = millis();
  lastScanTime = millis();
  state = LOGGING;

  WiFi.scanDelete();
  WiFi.scanNetworks(true);

  LOG_DBG("HMAP", "Started logging to %s", filename);
}

void WifiHeatMapActivity::stopLogging() {
  if (state != LOGGING) return;
  WiFi.scanDelete();
  LOG_DBG("HMAP", "Stopped. %d samples, %d points, %d unique APs", sampleCount, totalDataPoints, seenCount);
}

void WifiHeatMapActivity::processScanResults() {
  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;

  currentCount = 0;

  if (result > 0) {
    sampleCount++;
    unsigned long now = millis();

    FsFile f = Storage.open(filename, O_WRITE | O_CREAT | O_APPEND);

    for (int i = 0; i < result && i < MAX_CURRENT; i++) {
      uint8_t* rawBssid = WiFi.BSSID(i);
      char bssidBuf[18];
      snprintf(bssidBuf, sizeof(bssidBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
               rawBssid[0], rawBssid[1], rawBssid[2], rawBssid[3], rawBssid[4], rawBssid[5]);

      // Fill current readings for display
      ApReading& ap = currentReadings[currentCount];
      strncpy(ap.bssid, bssidBuf, sizeof(ap.bssid) - 1);
      ap.bssid[sizeof(ap.bssid) - 1] = '\0';

      String ssidStr = WiFi.SSID(i);
      if (ssidStr.isEmpty()) {
        strncpy(ap.ssid, "(hidden)", sizeof(ap.ssid) - 1);
      } else {
        strncpy(ap.ssid, ssidStr.c_str(), sizeof(ap.ssid) - 1);
      }
      ap.ssid[sizeof(ap.ssid) - 1] = '\0';

      ap.rssi = static_cast<int8_t>(WiFi.RSSI(i));
      ap.channel = static_cast<uint8_t>(WiFi.channel(i));
      currentCount++;

      // Track unique BSSIDs
      if (!isBssidSeen(bssidBuf) && seenCount < MAX_UNIQUE) {
        strncpy(seenBssids[seenCount], bssidBuf, sizeof(seenBssids[0]) - 1);
        seenBssids[seenCount][sizeof(seenBssids[0]) - 1] = '\0';
        seenCount++;
      }

      // HEATMAP-001: stop writing if we've hit the data point cap
      if (totalDataPoints >= MAX_DATA_POINTS) {
        if (f) f.close();
        WiFi.scanDelete();
        maxPointsReached = true;
        state = SUMMARY;
        LOG_DBG("HMAP", "Max data points reached, stopping logging");
        requestUpdate();
        return;
      }

      // HEATMAP-002: quote BSSID and SSID fields to handle commas/special chars
      if (f) {
        char line[128];
        const char* ssidToWrite = ssidStr.isEmpty() ? "" : ap.ssid;
        snprintf(line, sizeof(line), "%d,%lu,\"%s\",\"%s\",%d,%u\n",
                 sampleCount, now, bssidBuf, ssidToWrite, (int)ap.rssi, (unsigned)ap.channel);
        f.print(line);
        totalDataPoints++;
      }
    }

    if (f) f.close();
  }

  WiFi.scanDelete();
  lastScanTime = millis();
  requestUpdate();
}

void WifiHeatMapActivity::loop() {
  if (state == LOGGING) {
    processScanResults();

    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING &&
        millis() - lastScanTime >= SCAN_INTERVAL_MS) {
      WiFi.scanNetworks(true);
    }
  }

  buttonNavigator.onNext([this] {
    if (currentCount > 0) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, currentCount);
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentCount > 0) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, currentCount);
      requestUpdate();
    }
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == IDLE) {
      startLogging();
      requestUpdate();
    } else if (state == LOGGING) {
      stopLogging();
      state = SUMMARY;
      requestUpdate();
    } else if (state == SUMMARY) {
      finish();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == LOGGING) stopLogging();
    finish();
  }
}

void WifiHeatMapActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state == IDLE) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "WiFi Heat Map");

    int y = contentTop + 20;
    if (headerWriteFailed) {
      // HEATMAP-003: surface the write error to the user
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Error: could not write to SD card", true);
      y += 24;
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Check card and try again", true);
      y += 40;
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Walk around while logging RSSI", true);
      y += 24;
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Data saved to SD card as CSV", true);
      y += 40;
    }
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Press OK to start", true);

    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == LOGGING) {
    char subtitleBuf[40];
    snprintf(subtitleBuf, sizeof(subtitleBuf), "%d samples | %d points", sampleCount, totalDataPoints);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Heat Map", subtitleBuf);

    if (currentCount == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Scanning...", true);
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight},
          currentCount, selectorIndex,
          [this](int index) -> std::string {
            return currentReadings[index].ssid;
          },
          [this](int index) -> std::string {
            const auto& ap = currentReadings[index];
            char buf[40];
            snprintf(buf, sizeof(buf), "%s  %d dBm  Ch%u",
                     ap.bssid, (int)ap.rssi, (unsigned)ap.channel);
            return buf;
          });
    }

    const auto labels = mappedInput.mapLabels("Exit", "Done", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else {  // SUMMARY
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Heat Map — Summary");

    unsigned long duration = (millis() - startTime) / 1000;
    int y = contentTop + 16;

    char buf[64];
    snprintf(buf, sizeof(buf), "Samples: %d", sampleCount);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf, true);
    y += 24;

    snprintf(buf, sizeof(buf), "Data points: %d", totalDataPoints);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf, true);
    y += 24;

    snprintf(buf, sizeof(buf), "Unique APs: %d", seenCount);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf, true);
    y += 24;

    snprintf(buf, sizeof(buf), "Duration: %lus", duration);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf, true);
    y += 32;

    // HEATMAP-001: notify user when logging stopped due to the data point cap
    if (maxPointsReached) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Max samples reached", true);
      y += 24;
    }

    // Show just the filename portion (after last slash)
    const char* fname = filename;
    const char* slash = strrchr(filename, '/');
    if (slash) fname = slash + 1;
    renderer.drawCenteredText(SMALL_FONT_ID, y, fname, true);

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
