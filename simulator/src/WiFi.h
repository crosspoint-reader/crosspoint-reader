#pragma once
#include "NetworkClient.h"
#include "WString.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// On ESP32, WiFiClient is a TCP client backed by the WiFi stack.
// In the simulator NetworkClient already provides the same interface.
using WiFiClient = NetworkClient;

enum wl_status_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6
};

enum wifi_mode_t {
  WIFI_OFF = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3,
  WIFI_MODE_NULL = 0
};
enum wifi_auth_mode_t { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 };

#define WIFI_MODE_STA WIFI_STA
#define WIFI_MODE_AP WIFI_AP

class IPAddress {
  uint8_t bytes[4] = {0, 0, 0, 0};

public:
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
  }
  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2],
             bytes[3]);
    return String(buf);
  }
  uint8_t operator[](int i) const { return bytes[i % 4]; }
  uint8_t &operator[](int i) { return bytes[i % 4]; }
  bool operator==(const IPAddress &o) const {
    return memcmp(bytes, o.bytes, sizeof(bytes)) == 0;
  }
  bool operator!=(const IPAddress &o) const { return !(*this == o); }
};

class WiFiClass {
  struct Network {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t auth;
  };

  wifi_mode_t currentMode = WIFI_OFF;
  wl_status_t currentStatus = WL_DISCONNECTED;
  String currentSsid;
  std::vector<Network> defaultNetworks{
      {"Simulator WiFi (fake)", -45, WIFI_AUTH_OPEN},
      {"Local Test Network (fake)", -62, WIFI_AUTH_WPA2_PSK}};
  mutable bool cachedNetworkSpecInitialized = false;
  mutable std::string cachedNetworkSpec;
  mutable std::vector<Network> cachedConfiguredNetworks;

  static const char *envValue(const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] ? value : nullptr;
  }

  static const char *simEnvValue(const char *simName, const char *legacyName) {
    const char *value = envValue(simName);
    return value ? value : envValue(legacyName);
  }

  static bool simEnvEquals(const char *simName, const char *legacyName,
                           const char *expected) {
    const char *value = simEnvValue(simName, legacyName);
    return value && std::string(value) == expected;
  }

  bool scanOverrideEnabled() const {
    return simEnvValue("CROSSPOINT_SIM_WIFI_NETWORKS",
                       "CROSSPOINT_EMU_WIFI_NETWORKS") != nullptr;
  }

  const std::vector<Network> &configuredNetworks() const {
    const char *configured = simEnvValue("CROSSPOINT_SIM_WIFI_NETWORKS",
                                         "CROSSPOINT_EMU_WIFI_NETWORKS");
    const std::string spec =
        configured ? std::string(configured) : std::string();
    if (cachedNetworkSpecInitialized && cachedNetworkSpec == spec) {
      return cachedConfiguredNetworks;
    }

    cachedNetworkSpecInitialized = true;
    cachedNetworkSpec = spec;
    cachedConfiguredNetworks.clear();

    if (!configured) {
      cachedConfiguredNetworks = defaultNetworks;
      return cachedConfiguredNetworks;
    }
    if (spec == "none") {
      return cachedConfiguredNetworks;
    }

    size_t start = 0;
    while (start < spec.size()) {
      const size_t end = spec.find(';', start);
      std::string item = spec.substr(
          start, end == std::string::npos ? std::string::npos : end - start);
      if (!item.empty()) {
        Network network;
        size_t firstColon = item.find(':');
        size_t secondColon = firstColon == std::string::npos
                                 ? std::string::npos
                                 : item.find(':', firstColon + 1);
        network.ssid = String(item.substr(0, firstColon).c_str());
        network.rssi = firstColon == std::string::npos
                           ? -55
                           : std::atoi(item.substr(firstColon + 1,
                                                   secondColon - firstColon - 1)
                                           .c_str());
        network.auth = (secondColon != std::string::npos &&
                        item.substr(secondColon + 1) == "open")
                           ? WIFI_AUTH_OPEN
                           : WIFI_AUTH_WPA2_PSK;
        cachedConfiguredNetworks.push_back(std::move(network));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return cachedConfiguredNetworks;
  }

  bool ssidInConfiguredScan(const String &ssid) const {
    for (const auto &network : configuredNetworks()) {
      if (std::string(network.ssid.c_str()) == std::string(ssid.c_str())) {
        return true;
      }
    }
    return false;
  }

public:
  wl_status_t begin(const char *ssid = nullptr, const char *pass = nullptr) {
    (void)pass;
    currentMode = WIFI_STA;
    currentSsid = ssid ? ssid : "Simulator WiFi (fake)";
    if (simEnvEquals("CROSSPOINT_SIM_WIFI_CONNECT",
                     "CROSSPOINT_EMU_WIFI_CONNECT", "fail")) {
      currentStatus = WL_CONNECT_FAILED;
      return currentStatus;
    }
    if (simEnvEquals("CROSSPOINT_SIM_WIFI_CONNECT",
                     "CROSSPOINT_EMU_WIFI_CONNECT", "no-ssid")) {
      currentStatus = WL_NO_SSID_AVAIL;
      return currentStatus;
    }
    if (simEnvEquals("CROSSPOINT_SIM_WIFI_CONNECT",
                     "CROSSPOINT_EMU_WIFI_CONNECT", "timeout")) {
      currentStatus = WL_IDLE_STATUS;
      return currentStatus;
    }
    if (scanOverrideEnabled() && !ssidInConfiguredScan(currentSsid)) {
      currentStatus = WL_NO_SSID_AVAIL;
      return currentStatus;
    }
    currentStatus = WL_CONNECTED;
    return currentStatus;
  }
  wl_status_t status() { return currentStatus; }
  IPAddress localIP() {
    return currentStatus == WL_CONNECTED ? IPAddress(127, 0, 0, 1)
                                         : IPAddress();
  }
  void persistent(bool) {}
  void disconnect(bool wifioff = false, bool eraseap = false) {
    (void)wifioff;
    (void)eraseap;
    currentStatus = WL_DISCONNECTED;
  }
  void mode(int mode) {
    currentMode = static_cast<wifi_mode_t>(mode);
    if (mode == WIFI_OFF)
      currentStatus = WL_DISCONNECTED;
  }
  bool softAP(const char *ssid, const char *pass = NULL, int channel = 1,
              int hidden = 0, int max_connection = 4) {
    (void)pass;
    (void)channel;
    (void)hidden;
    (void)max_connection;
    currentMode = WIFI_AP;
    currentSsid = ssid ? ssid : "CrossVi-Simulator";
    if (simEnvEquals("CROSSPOINT_SIM_WIFI_AP", "CROSSPOINT_EMU_WIFI_AP",
                     "fail")) {
      currentStatus = WL_CONNECT_FAILED;
      return false;
    }
    currentStatus = WL_CONNECTED;
    return true;
  }
  bool softAPdisconnect(bool wifioff = false) {
    (void)wifioff;
    currentStatus = WL_DISCONNECTED;
    return true;
  }
  IPAddress softAPIP() { return IPAddress(127, 0, 0, 1); }

  String macAddress() { return String("02:00:00:00:00:01"); }
  uint8_t *macAddress(uint8_t *mac) {
    if (mac) {
      const std::array<uint8_t, 6> value{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
      memcpy(mac, value.data(), value.size());
    }
    return mac;
  }

  void scanDelete() {}
  int scanNetworks(bool async = false, bool show_hidden = false,
                   bool passive = false, uint32_t max_ms_per_chan = 300,
                   uint8_t channel = 0) {
    (void)async;
    (void)show_hidden;
    (void)passive;
    (void)max_ms_per_chan;
    (void)channel;
    return static_cast<int>(configuredNetworks().size());
  }
  int scanComplete() { return static_cast<int>(configuredNetworks().size()); }
  String SSID() {
    return currentSsid.isEmpty() ? String("Simulator WiFi (fake)")
                                 : currentSsid;
  }
  String SSID(int i) {
    const auto &networks = configuredNetworks();
    return i >= 0 && i < static_cast<int>(networks.size()) ? networks[i].ssid
                                                           : String();
  }
  int RSSI() { return -45; }
  int RSSI(int i) {
    const auto &networks = configuredNetworks();
    return i >= 0 && i < static_cast<int>(networks.size()) ? networks[i].rssi
                                                           : 0;
  }
  int encryptionType(int i) {
    const auto &networks = configuredNetworks();
    return i >= 0 && i < static_cast<int>(networks.size()) ? networks[i].auth
                                                           : WIFI_AUTH_OPEN;
  }
  void setHostname(const char *) {}
  wifi_mode_t getMode() { return currentMode; }
  void setSleep(bool) {}
  void setAutoReconnect(bool) {}
  String getHostname() { return String("crosspoint-simulator"); }
  int softAPgetStationNum() { return 0; }
};
extern WiFiClass WiFi;

#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2
