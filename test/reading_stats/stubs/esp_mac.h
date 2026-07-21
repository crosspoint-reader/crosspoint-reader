#pragma once

#include <cstddef>
#include <cstdint>

inline bool esp_mac_test_fail = false;

inline void setEspMacFailureForTest(const bool fail) { esp_mac_test_fail = fail; }

inline int esp_efuse_mac_get_default(uint8_t* mac) {
  if (!mac || esp_mac_test_fail) return -1;
  constexpr uint8_t DEVICE_MAC[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  for (size_t i = 0; i < 6; ++i) mac[i] = DEVICE_MAC[i];
  return 0;
}
