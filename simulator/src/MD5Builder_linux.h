#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WString.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(__linux__)
#include <openssl/md5.h>
#else
#error "Unsupported host OS for simulator MD5Builder"
#endif

class MD5Builder {
public:
  MD5Builder() { memset(digest_, 0, sizeof(digest_)); }

  void begin() {
#if defined(__APPLE__)
    CC_MD5_Init(&ctx_);
#else
    MD5_Init(&ctx_);
#endif
  }

  void add(const uint8_t *data, size_t len) {
#if defined(__APPLE__)
    CC_MD5_Update(&ctx_, data, static_cast<CC_LONG>(len));
#else
    MD5_Update(&ctx_, data, len);
#endif
  }

  void add(const char *str) {
    if (str)
      add(reinterpret_cast<const uint8_t *>(str), strlen(str));
  }

  void calculate() {
#if defined(__APPLE__)
    CC_MD5_Final(digest_, &ctx_);
#else
    MD5_Final(digest_, &ctx_);
#endif
  }

  String toString() const {
    char hex[33];
    for (int i = 0; i < 16; i++) {
      snprintf(hex + i * 2, 3, "%02x", digest_[i]);
    }
    return String(hex);
  }

private:
#if defined(__APPLE__)
  CC_MD5_CTX ctx_{};
#else
  MD5_CTX ctx_{};
#endif
  uint8_t digest_[16];
};
