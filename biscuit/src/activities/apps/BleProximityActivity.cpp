#include "BleProximityActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "util/RadioManager.h"

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Static pointer for callback context
static BleProximityActivity* activeProximity = nullptr;

class BleProximityCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!activeProximity) return;
    // Results are handled in the main loop via BLEScan results
  }
};

void BleProximityActivity::onEnter() {
  Activity::onEnter();
  devices.clear();
  selectorIndex = 0;
  scanning = false;
  scanInitialized = false;
  needsInit = true;
  requestUpdate();
}

void BleProximityActivity::onExit() {
  Activity::onExit();
  if (scanInitialized) {
    BLEScan* scan = BLEDevice::getScan();
    scan->stop();
    scanning = false;
  }
  activeProximity = nullptr;
  if (scanInitialized) {
    RADIO.shutdown();
    scanInitialized = false;
  }
}

void BleProximityActivity::startBleScan() {
  if (!scanInitialized) return;
  scanning = true;
  lastScanTime = millis();

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->clearResults();
  scan->start(0, nullptr, true);  // non-blocking
}

void BleProximityActivity::pruneStale() {
  const unsigned long now = millis();
  devices.erase(
      std::remove_if(devices.begin(), devices.end(),
                     [now](const BleTarget& d) {
                       return (now - d.lastSeen) > STALE_TIMEOUT_MS;
                     }),
      devices.end());
}

void BleProximityActivity::processScanResults() {
  // Not used — results are processed inline in loop() for clarity
}

void BleProximityActivity::loop() {
  if (needsInit) {
    needsInit = false;
    RADIO.ensureBle();
    scanInitialized = true;
    activeProximity = this;
    startBleScan();
    requestUpdate();
    return;
  }

  if (scanning && scanInitialized) {
    BLEScan* scan = BLEDevice::getScan();
    BLEScanResults* results = scan->getResults();
    const int count = results ? results->getCount() : 0;

    if (count > 0 || (millis() - lastScanTime > 4000)) {
      const unsigned long now = millis();

      // Mark all existing devices as inactive before merging
      for (auto& d : devices) {
        d.active = false;
      }

      // Merge results: update existing or add new
      for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice dev = results->getDevice(i);
        std::string mac = dev.getAddress().toString().c_str();

        auto it = std::find_if(devices.begin(), devices.end(),
                               [&mac](const BleTarget& d) { return d.mac == mac; });
        if (it != devices.end()) {
          it->rssi = dev.getRSSI();
          it->lastSeen = now;
          it->active = true;
          if (dev.haveName() && it->name == "Unknown") {
            it->name = dev.getName().c_str();
          }
        } else if (static_cast<int>(devices.size()) < MAX_DEVICES) {
          BleTarget newDev;
          newDev.name = dev.haveName() ? dev.getName().c_str() : "Unknown";
          newDev.mac = mac;
          newDev.rssi = dev.getRSSI();
          newDev.lastSeen = now;
          newDev.active = true;
          devices.push_back(std::move(newDev));
        }
      }

      // Remove devices not seen within the stale timeout
      pruneStale();

      // Sort by RSSI descending (strongest signal first)
      std::sort(devices.begin(), devices.end(),
                [](const BleTarget& a, const BleTarget& b) { return a.rssi > b.rssi; });

      // Clamp selector index to valid range
      const int devCount = static_cast<int>(devices.size());
      if (selectorIndex >= devCount && devCount > 0) {
        selectorIndex = devCount - 1;
      }

      scan->clearResults();
      requestUpdate();

      // Restart scan (non-blocking)
      lastScanTime = millis();
      scan->start(0, nullptr, true);
    }
  }

  // Navigation
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
    finish();
  }
}

void BleProximityActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (needsInit) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "BLE Proximity");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Starting BLE...");
    renderer.displayBuffer();
    return;
  }

  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "%zu devices", devices.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "BLE Proximity", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (devices.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SCANNING_BLE));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(devices.size()), selectorIndex,
        [this](int index) -> std::string {
          return devices[index].name;
        },
        [this](int index) -> std::string {
          char buf[64];
          snprintf(buf, sizeof(buf), "%s  %d dBm%s",
                   devices[index].mac.c_str(),
                   static_cast<int>(devices[index].rssi),
                   devices[index].active ? "" : " (stale)");
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
