#pragma once
#include "Arduino.h"

enum wl_status_t { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum WiFiMode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3, WIFI_MODE_NULL = 0 };

class IPAddress {
  uint8_t bytes[4] = {0, 0, 0, 0};

 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes{a, b, c, d} {}
  bool operator!=(const IPAddress& other) const { return memcmp(bytes, other.bytes, sizeof(bytes)) != 0; }
  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return String(buf);
  }
};

class WiFiClass {
 public:
  void mode(WiFiMode_t) {}
  WiFiMode_t getMode() const { return WIFI_OFF; }
  wl_status_t status() const { return WL_DISCONNECTED; }
  IPAddress localIP() const { return IPAddress(); }
  void disconnect(bool = true) {}
};

extern WiFiClass WiFi;
