#if CROSSPOINT_EMULATED

#include <cstdint>

extern "C" {
uint32_t uzlib_adler32(const void* data, unsigned int length, uint32_t prev_sum) {
  constexpr uint32_t mod = 65521;
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t a = prev_sum & 0xffff;
  uint32_t b = (prev_sum >> 16) & 0xffff;
  for (unsigned int i = 0; i < length; ++i) {
    a = (a + bytes[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (unsigned int i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}
}

#endif
