#pragma once

constexpr int WIFI_PS_NONE = 0;
constexpr int WIFI_PS_MIN_MODEM = 1;
inline void esp_wifi_set_ps(int) {}
