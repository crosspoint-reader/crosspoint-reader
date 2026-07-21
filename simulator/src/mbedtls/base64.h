#pragma once
#include <cstddef>
#include <cstdint>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
#define MBEDTLS_ERR_BASE64_INVALID_CHARACTER -0x002C

// Minimal base64 decode stub for simulator builds.
static inline int mbedtls_base64_decode(unsigned char *dst, size_t dlen,
                                        size_t *olen, const unsigned char *src,
                                        size_t slen) {
  static const signed char dec[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57,
      58, 59, 60, 61, -1, -1, -1, -2, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
      7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
      25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
      37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1,
  };

  size_t i, n = 0;
  unsigned char c0, c1, c2, c3;

  // Count padding to determine output length
  size_t pad = 0;
  for (i = slen; i > 0 && src[i - 1] == '='; i--)
    pad++;
  size_t outLen = (slen / 4) * 3 - pad;

  if (dst == nullptr) {
    *olen = outLen;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  if (dlen < outLen) {
    *olen = outLen;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }

  *olen = 0;
  for (i = 0; i + 3 < slen; i += 4) {
    c0 = (unsigned char)dec[(unsigned char)src[i]];
    c1 = (unsigned char)dec[(unsigned char)src[i + 1]];
    c2 = (unsigned char)dec[(unsigned char)src[i + 2]];
    c3 = (unsigned char)dec[(unsigned char)src[i + 3]];
    if ((signed char)c0 < 0 || (signed char)c1 < 0)
      return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    dst[n++] = (c0 << 2) | (c1 >> 4);
    if (src[i + 2] != '=') {
      if ((signed char)c2 < 0)
        return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
      dst[n++] = (c1 << 4) | (c2 >> 2);
    }
    if (src[i + 3] != '=') {
      if ((signed char)c3 < 0)
        return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
      dst[n++] = (c2 << 6) | c3;
    }
  }
  *olen = n;
  return 0;
}
