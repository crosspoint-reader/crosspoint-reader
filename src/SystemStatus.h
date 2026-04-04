#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "HalPowerManager.h"

// Snapshot of device system status, shared between the web server and the
// System Information activity so both surfaces show consistent data.
struct SystemStatus {
  const char* version;
  std::string chipVersion;
  uint32_t cpuFreqMHz;
  std::string ip;
  std::string wifiMode;  // "STA", "AP", or "Off"
  int rssi;              // dBm; 0 when not in STA mode
  std::string macAddress;
  uint32_t freeHeapBytes;
  uint32_t minFreeHeapBytes;
  uint32_t maxAllocHeapBytes;
  uint64_t flashBytes;
  uint64_t flashAppUsedBytes;
  uint64_t flashAppFreeBytes;
  uint16_t batteryPercent;
  bool charging;
  uint32_t uptimeSeconds;
  uint64_t sdTotalBytes;
  uint64_t sdUsedBytes;
  uint64_t sdFreeBytes;

  static SystemStatus collectFast() {
    SystemStatus s;
    s.version = CROSSPOINT_VERSION;
    s.chipVersion = ESP.getChipModel();
    s.chipVersion += " rev ";
    s.chipVersion += std::to_string(ESP.getChipRevision());
    s.cpuFreqMHz = static_cast<uint32_t>(getCpuFrequencyMhz());
    s.freeHeapBytes = ESP.getFreeHeap();
    s.minFreeHeapBytes = ESP.getMinFreeHeap();
    s.maxAllocHeapBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s.flashBytes = static_cast<uint64_t>(ESP.getFlashChipSize());
    s.flashAppUsedBytes = static_cast<uint64_t>(ESP.getSketchSize());
    s.flashAppFreeBytes = static_cast<uint64_t>(ESP.getFreeSketchSpace());
    s.batteryPercent = powerManager.getBatteryPercentage();
    s.charging = digitalRead(UART0_RXD) == HIGH;
    s.uptimeSeconds = millis() / 1000;
    s.macAddress = WiFi.macAddress().c_str();
    s.sdTotalBytes = 0;
    s.sdUsedBytes = 0;
    s.sdFreeBytes = 0;

    const wifi_mode_t mode = WiFi.getMode();
    const bool isAP = (mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA);

    if (isAP) {
      s.wifiMode = "AP";
      s.ip = WiFi.softAPIP().toString().c_str();
      s.rssi = 0;
    } else if (WiFi.status() == WL_CONNECTED) {
      s.wifiMode = "STA";
      s.ip = WiFi.localIP().toString().c_str();
      s.rssi = WiFi.RSSI();
    } else {
      s.wifiMode = "Off";
      s.ip = "-";
      s.rssi = 0;
    }

    return s;
  }

  static void fillSdStatus(SystemStatus& s) {
    s.sdTotalBytes = Storage.sdTotalBytes();
    s.sdUsedBytes = Storage.sdUsedBytes();
    s.sdFreeBytes = Storage.sdFreeBytes();
  }

  static SystemStatus collect() {
    SystemStatus s = collectFast();
    fillSdStatus(s);
    return s;
  }
};
