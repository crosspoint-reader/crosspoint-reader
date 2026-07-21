#pragma once
#include <cstdint>
#include <cstring>

// Simulator stub: return a fixed fake MAC address
static inline int esp_efuse_mac_get_default(uint8_t mac[6]) {
  static const uint8_t fakeMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  memcpy(mac, fakeMac, 6);
  return 0;
}
