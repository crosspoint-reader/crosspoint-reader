#include <stddef.h>
#include <stdint.h>

// Minimal stubs for host linking; PDF FlateDecode paths used by InflateReader do not rely on these.
uint32_t uzlib_adler32(const void* data, unsigned int length, uint32_t prev_sum) {
  (void)data;
  (void)length;
  return prev_sum;
}

uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  (void)data;
  (void)length;
  return crc;
}
