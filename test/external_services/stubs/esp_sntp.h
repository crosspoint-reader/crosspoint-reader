#pragma once

inline bool fakeSntpRunning = false;
inline int fakeSntpStops = 0;

inline bool esp_sntp_enabled() { return fakeSntpRunning; }
inline void esp_sntp_stop() {
  fakeSntpRunning = false;
  ++fakeSntpStops;
}
