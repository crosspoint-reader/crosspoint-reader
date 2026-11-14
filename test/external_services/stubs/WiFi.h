#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a, b, c, d} {}

  uint8_t operator[](size_t index) const { return bytes_[index]; }

  bool fromString(const char* text) {
    unsigned values[4];
    char tail = '\0';
    if (!text || std::sscanf(text, "%u.%u.%u.%u%c", &values[0], &values[1], &values[2], &values[3], &tail) != 4)
      return false;
    for (unsigned value : values)
      if (value > 255) return false;
    for (size_t i = 0; i < 4; ++i) bytes_[i] = static_cast<uint8_t>(values[i]);
    return true;
  }

 private:
  std::array<uint8_t, 4> bytes_{};
};

using wl_status_t = int;
inline constexpr wl_status_t WL_DISCONNECTED = 0;
inline constexpr wl_status_t WL_CONNECTED = 3;

enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA };

class FakeWiFiClass {
 public:
  wl_status_t status() const { return status_; }
  wifi_mode_t getMode() const { return mode_; }
  IPAddress localIP() const { return local_; }
  IPAddress subnetMask() const { return mask_; }
  IPAddress dnsIP(int) const { return dns_; }
  IPAddress softAPIP() const { return ap_; }

  wl_status_t status_ = WL_DISCONNECTED;
  wifi_mode_t mode_ = WIFI_MODE_NULL;
  IPAddress local_;
  IPAddress mask_;
  IPAddress dns_;
  IPAddress ap_;
};

inline FakeWiFiClass WiFi;
