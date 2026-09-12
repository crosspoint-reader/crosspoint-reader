#pragma once

constexpr int WL_CONNECTED = 3;
constexpr int WL_DISCONNECTED = 6;

class WiFiClass {
 public:
  int status() const;
};

extern WiFiClass WiFi;
