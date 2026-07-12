#include "DeviceFingerprinterActivity.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// Static instance
// ---------------------------------------------------------------------------

DeviceFingerprinterActivity* DeviceFingerprinterActivity::activeInstance = nullptr;

void DeviceFingerprinterActivity::promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!activeInstance || !buf) return;
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = pkt->payload;
  const uint16_t sig_len = pkt->rx_ctrl.sig_len;

  if (sig_len < 16) return;

  // Probe request: FC byte 0 = 0x40
  if ((payload[0] & 0xFC) != 0x40) return;

  activeInstance->onProbePacket(payload, sig_len, pkt->rx_ctrl.rssi);
}

// ---------------------------------------------------------------------------
// OS fingerprint heuristic
// ---------------------------------------------------------------------------

const char* DeviceFingerprinterActivity::estimateOs(const uint8_t* mac, int probeCount) {
  const bool randomized = (mac[0] & 0x02) != 0;  // locally administered bit

  if (randomized) {
    // iOS tends to send very few probe requests; Android sends more
    return (probeCount <= 3) ? "iOS" : "Android";
  }

  // Known OUI prefixes
  // Intel NICs (common in Windows/Linux laptops)
  if ((mac[0] == 0x00 && mac[1] == 0x1B && mac[2] == 0x21) ||
      (mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x67) ||
      (mac[0] == 0x8C && mac[1] == 0xEC && mac[2] == 0x4B) ||
      (mac[0] == 0xA4 && mac[1] == 0xC3 && mac[2] == 0xF0)) {
    return "Windows";
  }

  // Realtek (common in Windows/Linux)
  if ((mac[0] == 0x00 && mac[1] == 0xE0 && mac[2] == 0x4C) ||
      (mac[0] == 0x48 && mac[1] == 0x02 && mac[2] == 0x2A)) {
    return "Windows";
  }

  // Raspberry Pi / Linux embedded
  if ((mac[0] == 0xB8 && mac[1] == 0x27 && mac[2] == 0xEB) ||
      (mac[0] == 0xDC && mac[1] == 0xA6 && mac[2] == 0x32) ||
      (mac[0] == 0xE4 && mac[1] == 0x5F && mac[2] == 0x01)) {
    return "Linux";
  }

  return "Unknown";
}

// ---------------------------------------------------------------------------
// onProbePacket — called from ISR-level callback (keep minimal)
// ---------------------------------------------------------------------------

void DeviceFingerprinterActivity::onProbePacket(const uint8_t* payload, uint16_t len, int rssi) {
  (void)len;
  const uint8_t* srcMac = payload + 10;

  portENTER_CRITICAL(&dataMux);

  // Search for existing entry
  for (auto& dev : devices) {
    if (memcmp(dev.mac, srcMac, 6) == 0) {
      dev.probeCount++;
      dev.rssi = static_cast<int8_t>(rssi);
      const char* os = estimateOs(srcMac, dev.probeCount);
      strncpy(dev.estimatedOs, os, sizeof(dev.estimatedOs) - 1);
      dev.estimatedOs[sizeof(dev.estimatedOs) - 1] = '\0';
      portEXIT_CRITICAL(&dataMux);
      return;
    }
  }

  // New device
  if (static_cast<int>(devices.size()) >= MAX_DEVICES) {
    portEXIT_CRITICAL(&dataMux);
    return;
  }

  FingerprintedDevice d{};
  memcpy(d.mac, srcMac, 6);
  d.probeCount = 1;
  d.rssi = static_cast<int8_t>(rssi);
  const char* os = estimateOs(srcMac, 1);
  strncpy(d.estimatedOs, os, sizeof(d.estimatedOs) - 1);
  d.estimatedOs[sizeof(d.estimatedOs) - 1] = '\0';
  devices.push_back(d);

  portEXIT_CRITICAL(&dataMux);
}

// ---------------------------------------------------------------------------
// onEnter / onExit
// ---------------------------------------------------------------------------

void DeviceFingerprinterActivity::onEnter() {
  Activity::onEnter();
  state = READY;
  devices.clear();
  devices.reserve(MAX_DEVICES);
  deviceIndex = 0;
  promiscuousActive = false;
  requestUpdate();
}

void DeviceFingerprinterActivity::startCapture() {
  lastDisplay = millis();

  RADIO.ensureWifi();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  activeInstance = this;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);

  promiscuousActive = true;
  state = CAPTURING;
  requestUpdate();
}

void DeviceFingerprinterActivity::onExit() {
  Activity::onExit();
  if (promiscuousActive) {
    esp_wifi_set_promiscuous(false);
    promiscuousActive = false;
  }
  activeInstance = nullptr;
  RADIO.shutdown();
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void DeviceFingerprinterActivity::loop() {
  if (state == READY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startCapture();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  const unsigned long now = millis();

  if (now - lastDisplay >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    requestUpdate();
  }

  portENTER_CRITICAL(&dataMux);
  const int count = static_cast<int>(devices.size());
  portEXIT_CRITICAL(&dataMux);

  buttonNavigator.onNext([this, count] {
    if (count > 0) {
      deviceIndex = ButtonNavigator::nextIndex(deviceIndex, count);
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this, count] {
    if (count > 0) {
      deviceIndex = ButtonNavigator::previousIndex(deviceIndex, count);
      requestUpdate();
    }
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void DeviceFingerprinterActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == READY) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Device Fingerprinter");
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 30, "Identify nearby device types");
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, "by analyzing WiFi probe requests.");
    renderer.drawCenteredText(SMALL_FONT_ID, centerY + 40, "Press Confirm to start capture.");
    const auto labels = mappedInput.mapLabels("Back", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  portENTER_CRITICAL(&dataMux);
  const auto devicesCopy = devices;
  portEXIT_CRITICAL(&dataMux);
  const int devCount = static_cast<int>(devicesCopy.size());

  char subtitle[24];
  snprintf(subtitle, sizeof(subtitle), "%d devices", devCount);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Device Fingerprinter", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (devCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Listening for probe requests...");
  } else {
    // Clamp index defensively
    if (deviceIndex >= devCount) deviceIndex = devCount - 1;

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        devCount, deviceIndex,
        [&devicesCopy](int i) -> std::string {
          const auto& d = devicesCopy[i];
          const bool randomized = (d.mac[0] & 0x02) != 0;
          if (randomized) {
            return std::string("Randomized  ") + d.estimatedOs;
          }
          char buf[32];
          snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                   d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
          return std::string(buf) + "  " + d.estimatedOs;
        },
        [&devicesCopy](int i) -> std::string {
          const auto& d = devicesCopy[i];
          char buf[32];
          snprintf(buf, sizeof(buf), "Probes: %d  RSSI: %d dBm",
                   d.probeCount, static_cast<int>(d.rssi));
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels("Back", "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
