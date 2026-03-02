#pragma once
// Stub for native unit tests — no-op base64 decode
#include <cstddef>
#include <cstdint>
#include <cstring>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL (-0x002A)
#define MBEDTLS_ERR_BASE64_INVALID_CHARACTER (-0x002C)

inline int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen,
                                  const unsigned char* src, size_t slen) {
  if (!dst) {
    if (olen) *olen = slen;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  size_t n = slen < dlen ? slen : dlen;
  if (n > 0) memcpy(dst, src, n);
  if (olen) *olen = n;
  return 0;
}
