#include "TrackerDetectorActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void TrackerDetectorActivity::onEnter() {
  Activity::onEnter();
  state = IDLE;
  monitoring = false;
  scanInitialized = false;
  devices.clear();
  scanCycleCount = 0;
  alertCount = 0;
  selectorIndex = 0;
  requestUpdate();
}

void TrackerDetectorActivity::onExit() {
  Activity::onExit();
  if (scanInitialized) {
    BLEScan* scan = BLEDevice::getScan();
    scan->stop();
    RADIO.shutdown();
    scanInitialized = false;
  }
  monitoring = false;
}

void TrackerDetectorActivity::startMonitoring() {
  monitoring = true;
  state = MONITORING;
  scanCycleCount = 0;
  devices.clear();
  alertCount = 0;
  lastScanTime = 0;
  needsBleInit = true;
  requestUpdate();
}

void TrackerDetectorActivity::stopMonitoring() {
  if (scanInitialized) {
    BLEScan* scan = BLEDevice::getScan();
    scan->stop();
    RADIO.shutdown();
    scanInitialized = false;
  }
  monitoring = false;
  state = IDLE;
  requestUpdate();
}

void TrackerDetectorActivity::runScan() {
  if (!scanInitialized) return;
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->clearResults();
  scan->start(0, nullptr, true);  // non-blocking
  lastScanTime = millis();
}

void TrackerDetectorActivity::processScanResults() {
  BLEScan* scan = BLEDevice::getScan();
  BLEScanResults* results = scan->getResults();
  if (!results) return;

  for (int i = 0; i < results->getCount(); i++) {
    BLEAdvertisedDevice dev = results->getDevice(i);
    std::string mac = dev.getAddress().toString().c_str();

    TrackerType type = UNKNOWN_TRACKER;
    if (dev.haveManufacturerData()) {
      String mfRaw = dev.getManufacturerData();
      std::string svcStr = dev.haveServiceUUID() ? std::string(dev.getServiceUUID().toString().c_str()) : "";
      type = identifyTracker(
          reinterpret_cast<const uint8_t*>(mfRaw.c_str()), mfRaw.length(), svcStr);
    } else if (dev.haveServiceUUID()) {
      // Some trackers only advertise via service UUID
      type = identifyTracker(nullptr, 0, std::string(dev.getServiceUUID().toString().c_str()));
    }

    if (type == UNKNOWN_TRACKER) continue;

    auto it = std::find_if(devices.begin(), devices.end(),
                           [&mac](const TrackedDevice& d) { return d.mac == mac; });

    if (it != devices.end()) {
      it->rssi = dev.getRSSI();
      it->lastSeen = millis();
      it->seenCount++;
    } else if (static_cast<int>(devices.size()) < MAX_TRACKED) {
      devices.push_back({mac, type, static_cast<int8_t>(dev.getRSSI()), 1, millis(), millis(), false});
    }
  }

  scan->clearResults();
  scanCycleCount++;
}

void TrackerDetectorActivity::checkForFollowers() {
  alertCount = 0;
  for (auto& dev : devices) {
    unsigned long duration = dev.lastSeen - dev.firstSeen;
    if (dev.seenCount >= FOLLOW_THRESHOLD && duration >= FOLLOW_TIME_MS) {
      dev.flagged = true;
      alertCount++;
    }
  }
  if (alertCount > 0 && state != ALERT) {
    state = ALERT;
  }
}

void TrackerDetectorActivity::pruneStale() {
  const unsigned long now = millis();
  devices.erase(
      std::remove_if(devices.begin(), devices.end(),
                     [now](const TrackedDevice& d) { return (now - d.lastSeen) > STALE_TIMEOUT_MS; }),
      devices.end());
}

TrackerDetectorActivity::TrackerType TrackerDetectorActivity::identifyTracker(
    const uint8_t* mfData, size_t mfLen, const std::string& svcUuids) {
  if (mfData && mfLen >= 2) {
    uint16_t companyId = mfData[0] | (mfData[1] << 8);

    // Apple (0x004C) — AirTag and Find My
    if (companyId == 0x004C && mfLen >= 4) {
      uint8_t advType = mfData[2];
      if (advType == 0x12 && mfLen >= 5 && mfData[3] == 0x19) {
        return APPLE_AIRTAG;
      }
      if (advType == 0x12) {
        return APPLE_FINDMY;
      }
    }

    // Samsung (0x0075) — SmartTag
    if (companyId == 0x0075) {
      return SAMSUNG_SMARTTAG;
    }
  }

  // Tile — service UUID contains "feed"
  if (svcUuids.find("feed") != std::string::npos || svcUuids.find("FEED") != std::string::npos) {
    return TILE;
  }

  // Google Find My Device — Fast Pair service 0xFE2C
  if (svcUuids.find("fe2c") != std::string::npos || svcUuids.find("FE2C") != std::string::npos) {
    return GOOGLE_FINDER;
  }

  return UNKNOWN_TRACKER;
}

const char* TrackerDetectorActivity::trackerTypeName(TrackerType type) {
  switch (type) {
    case APPLE_AIRTAG:     return "AirTag";
    case APPLE_FINDMY:     return "Find My";
    case SAMSUNG_SMARTTAG: return "SmartTag";
    case TILE:             return "Tile";
    case GOOGLE_FINDER:    return "Google";
    default:               return "Unknown";
  }
}

void TrackerDetectorActivity::loop() {
  switch (state) {
    case IDLE:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startMonitoring();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;

    case MONITORING: {
      if (needsBleInit) {
        if (millis() - lastSpinnerUpdate >= 600) {
          lastSpinnerUpdate = millis();
          spinnerFrame = (spinnerFrame + 1) % 3;
          requestUpdate();
        }
        needsBleInit = false;
        RADIO.ensureBle();
        scanInitialized = true;
        runScan();
        requestUpdate();
        return;
      }

      // Process any available scan results each loop iteration
      processScanResults();

      // Periodic: trigger new scan, check followers, prune stale
      if (millis() - lastScanTime >= SCAN_INTERVAL_MS || lastScanTime == 0) {
        runScan();
        checkForFollowers();
        pruneStale();
        requestUpdate();
      }

      // Navigation through device list
      const int devCount = static_cast<int>(devices.size());
      buttonNavigator.onNext([this, devCount] {
        if (devCount > 0) {
          selectorIndex = ButtonNavigator::nextIndex(selectorIndex, devCount);
          requestUpdate();
        }
      });
      buttonNavigator.onPrevious([this, devCount] {
        if (devCount > 0) {
          selectorIndex = ButtonNavigator::previousIndex(selectorIndex, devCount);
          requestUpdate();
        }
      });

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        stopMonitoring();
      }
      break;
    }

    case ALERT: {
      const int devCount = static_cast<int>(devices.size());
      buttonNavigator.onNext([this, devCount] {
        if (devCount > 0) {
          selectorIndex = ButtonNavigator::nextIndex(selectorIndex, devCount);
          requestUpdate();
        }
      });
      buttonNavigator.onPrevious([this, devCount] {
        if (devCount > 0) {
          selectorIndex = ButtonNavigator::previousIndex(selectorIndex, devCount);
          requestUpdate();
        }
      });

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        // Acknowledge alert, return to monitoring
        state = MONITORING;
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        stopMonitoring();
      }
      break;
    }
  }
}

void TrackerDetectorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case IDLE:       renderIdle();       break;
    case MONITORING: renderMonitoring(); break;
    case ALERT:      renderAlert();      break;
  }
  renderer.displayBuffer();
}

void TrackerDetectorActivity::renderIdle() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Tracker Detector");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 40,
                            "Scans for nearby BLE trackers");
  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 10,
                            "(AirTag, SmartTag, Tile, Google)");
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 20,
                            "and alerts if one follows you.");
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 60,
                            "Press Confirm to start monitoring.");

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_START), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TrackerDetectorActivity::renderMonitoring() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (needsBleInit) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Tracker Detector");
    GUI.drawSpinner(renderer, pageWidth / 2, pageHeight / 2, "STARTING...", spinnerFrame);
    return;
  }

  char subtitle[64];
  unsigned long elapsed = millis() / 60000;  // minutes since boot
  snprintf(subtitle, sizeof(subtitle), "Cycle %u | %d trackers | %lum",
           scanCycleCount, static_cast<int>(devices.size()), elapsed);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Monitoring", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (devices.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2,
                              "No trackers detected yet...");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(devices.size()), selectorIndex,
        [this](int index) -> std::string {
          char buf[48];
          snprintf(buf, sizeof(buf), "%s  [%s]",
                   trackerTypeName(devices[index].type),
                   devices[index].flagged ? "ALERT" : "OK");
          return buf;
        },
        [this](int index) -> std::string {
          char buf[64];
          snprintf(buf, sizeof(buf), "%s  %ddBm  seen:%d",
                   devices[index].mac.c_str(),
                   static_cast<int>(devices[index].rssi),
                   static_cast<int>(devices[index].seenCount));
          return buf;
        });
  }

  // Status line: next scan countdown
  unsigned long sinceLastScan = millis() - lastScanTime;
  char statusBuf[32];
  if (sinceLastScan < SCAN_INTERVAL_MS) {
    unsigned long nextIn = (SCAN_INTERVAL_MS - sinceLastScan) / 1000;
    snprintf(statusBuf, sizeof(statusBuf), "Next scan in %lus", nextIn);
  } else {
    snprintf(statusBuf, sizeof(statusBuf), "Scanning...");
  }
  renderer.drawText(SMALL_FONT_ID, 16, pageHeight - metrics.buttonHintsHeight - 20, statusBuf);

  const auto labels = mappedInput.mapLabels(tr(STR_STOP), "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TrackerDetectorActivity::renderAlert() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Tracker Alert!");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Draw alert box
  renderer.fillRect(16, contentTop, pageWidth - 32, 30, true);
  char alertBuf[48];
  snprintf(alertBuf, sizeof(alertBuf), " %d tracker(s) may be following you!", alertCount);
  renderer.drawText(UI_10_FONT_ID, 24, contentTop + 6, alertBuf, false);

  const int listTop = contentTop + 40;
  const int contentHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Show flagged devices
  std::vector<int> flaggedIndices;
  for (int i = 0; i < static_cast<int>(devices.size()); i++) {
    if (devices[i].flagged) flaggedIndices.push_back(i);
  }

  if (!flaggedIndices.empty()) {
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, contentHeight},
        static_cast<int>(flaggedIndices.size()), selectorIndex,
        [this, &flaggedIndices](int index) -> std::string {
          int di = flaggedIndices[index];
          char buf[48];
          snprintf(buf, sizeof(buf), "%s  %s",
                   trackerTypeName(devices[di].type),
                   devices[di].mac.c_str());
          return buf;
        },
        [this, &flaggedIndices](int index) -> std::string {
          int di = flaggedIndices[index];
          char buf[64];
          unsigned long mins = (devices[di].lastSeen - devices[di].firstSeen) / 60000;
          snprintf(buf, sizeof(buf), "Seen %d times over %lum  %ddBm",
                   static_cast<int>(devices[di].seenCount), mins,
                   static_cast<int>(devices[di].rssi));
          return buf;
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_STOP), tr(STR_CONFIRM), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
