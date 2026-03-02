#pragma once
// Stub for native unit tests — returns deterministic zero MAC
#include <cstdint>

inline int esp_efuse_mac_get_default(uint8_t* mac) {
  for (int i = 0; i < 6; i++) mac[i] = 0;
  return 0;
}
