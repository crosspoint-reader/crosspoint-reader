#include <stdint.h>
#include <zlib.h>

uint32_t uzlib_adler32(const void* data, unsigned int length, uint32_t prev_sum) {
  return adler32(prev_sum, (const Bytef*)data, length);
}

uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  return crc32(crc, (const Bytef*)data, length);
}
