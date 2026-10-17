#pragma once
// Host stand-in for wolfCrypt's one-shot SHA-1, which the firmware links for TLS: a plain FIPS 180-4 SHA-1,
// checked against the standard's test vectors by BookFontsTest.

#include <cstddef>
#include <cstdint>

using byte = uint8_t;
using word32 = uint32_t;

inline int wc_ShaHash(const byte* data, const word32 len, byte* hash) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  const auto rol = [](const uint32_t x, const int n) { return (x << n) | (x >> (32 - n)); };
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  const size_t total = ((static_cast<size_t>(len) + 8) / 64 + 1) * 64;
  for (size_t block = 0; block < total; block += 64) {
    uint8_t chunk[64];
    for (size_t i = 0; i < 64; ++i) {
      const size_t at = block + i;
      if (at < len) {
        chunk[i] = data[at];
      } else if (at == len) {
        chunk[i] = 0x80;
      } else if (at >= total - 8) {
        chunk[i] = static_cast<uint8_t>(bits >> (8 * (total - 1 - at)));
      } else {
        chunk[i] = 0;
      }
    }
    uint32_t w[80];
    for (int t = 0; t < 16; ++t) {
      w[t] = (static_cast<uint32_t>(chunk[4 * t]) << 24) | (static_cast<uint32_t>(chunk[4 * t + 1]) << 16) |
             (static_cast<uint32_t>(chunk[4 * t + 2]) << 8) | chunk[4 * t + 3];
    }
    for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; ++t) {
      uint32_t f, k;
      if (t < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (t < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (t < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = rol(a, 5) + f + e + k + w[t];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 4; ++j) hash[4 * i + j] = static_cast<byte>(h[i] >> (24 - 8 * j));
  }
  return 0;
}
