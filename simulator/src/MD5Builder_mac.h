#pragma once
#include <CommonCrypto/CommonDigest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WString.h"

// Simulator implementation of Arduino ESP32's MD5Builder using macOS
// CommonCrypto.
class MD5Builder {
public:
  MD5Builder() { memset(digest_, 0, sizeof(digest_)); }

  void begin() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5_Init(&ctx_);
#pragma clang diagnostic pop
  }

  void add(const uint8_t *data, size_t len) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5_Update(&ctx_, data, static_cast<CC_LONG>(len));
#pragma clang diagnostic pop
  }

  void add(const char *str) {
    if (str)
      add(reinterpret_cast<const uint8_t *>(str), strlen(str));
  }

  void calculate() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5_Final(digest_, &ctx_);
#pragma clang diagnostic pop
  }

  String toString() const {
    char hex[33];
    for (int i = 0; i < 16; i++) {
      snprintf(hex + i * 2, 3, "%02x", digest_[i]);
    }
    return String(hex);
  }

private:
  CC_MD5_CTX ctx_{};
  uint8_t digest_[16];
};
