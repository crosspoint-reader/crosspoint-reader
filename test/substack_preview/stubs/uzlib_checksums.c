#include <stdint.h>

/* uzlib_uncompress_chksum() (lib/uzlib/src/tinflate.c) references these, but
 * substack_preview never calls it (only built-in, uncompressed fonts are
 * used) — real bodies aren't vendored in this trimmed lib/uzlib/ copy, so
 * these exist only to satisfy the linker. */
uint32_t uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum) {
  (void)data;
  (void)length;
  return prev_sum;
}

uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t crc) {
  (void)data;
  (void)length;
  return crc;
}
