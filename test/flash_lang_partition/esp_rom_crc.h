#pragma once
// The ROM CRC the firmware uses, as zlib computes it: chained from 0 (FontInstaller.h relies on the
// same equivalence for its manifests).

#include <cstdint>

inline uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const* buf, const uint32_t len) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
      table[i] = c;
    }
    built = true;
  }
  crc = ~crc;
  for (uint32_t i = 0; i < len; ++i) crc = table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}
