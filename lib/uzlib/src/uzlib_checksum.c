#include "uzlib.h"

uint32_t TINFCC uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum) {
  const uint8_t *buf = (const uint8_t *)data;
  uint32_t s1 = prev_sum & 0xffffu;
  uint32_t s2 = (prev_sum >> 16) & 0xffffu;
  while (length--) {
    s1 = (s1 + *buf++) % 65521u;
    s2 = (s2 + s1) % 65521u;
  }
  return (s2 << 16) | s1;
}

uint32_t TINFCC uzlib_crc32(const void *data, unsigned int length, uint32_t crc) {
  const uint8_t *buf = (const uint8_t *)data;
  while (length--) {
    crc ^= *buf++;
    for (int i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1u)));
    }
  }
  return crc;
}
