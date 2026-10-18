#pragma once
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int) { return CC_SHA256_Init(c) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const unsigned char* p, size_t n) {
  return CC_SHA256_Update(c, p, n) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, unsigned char* p) {
  return CC_SHA256_Final(p, c) == 1 ? 0 : -1;
}
