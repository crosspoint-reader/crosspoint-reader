#pragma once
#include <cstdint>

inline int esp_efuse_mac_get_default(uint8_t* mac) {
  for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(0xa0 + i);
  return 0;
}
