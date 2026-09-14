#pragma once

constexpr int WL_CONNECTED = 3;

struct WiFiHostStub {
  int status() const { return 0; }
};

inline WiFiHostStub WiFi;
