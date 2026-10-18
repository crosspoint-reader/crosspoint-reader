#pragma once
constexpr int WIFI_OFF = 0;
struct Wifi {
  void mode(int) {}
};
inline Wifi WiFi;
