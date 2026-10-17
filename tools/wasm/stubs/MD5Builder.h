#pragma once
// Adapted from CrossPlay (MIT); see ../LICENSE.crossplay.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "WString.h"

class MD5Builder {
 public:
  MD5Builder() { memset(digest_, 0, sizeof(digest_)); }

  void begin() {
    count_ = 0;
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bufLen_ = 0;
  }

  void add(const uint8_t* data, size_t len) {
    count_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
      size_t take = 64 - bufLen_;
      if (take > len) take = len;
      memcpy(buf_ + bufLen_, data, take);
      bufLen_ += take;
      data += take;
      len -= take;
      if (bufLen_ == 64) {
        transform(buf_);
        bufLen_ = 0;
      }
    }
  }

  void add(const char* str) {
    if (str) add(reinterpret_cast<const uint8_t*>(str), strlen(str));
  }

  void calculate() {
    static constexpr uint8_t kPad[64] = {0x80};
    const uint64_t bits = count_;
    size_t padLen = (bufLen_ < 56) ? (56 - bufLen_) : (120 - bufLen_);
    add(kPad, padLen);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++) tail[i] = static_cast<uint8_t>(bits >> (8 * i));
    // add() would re-count these bits, so splice them in directly.
    memcpy(buf_ + bufLen_, tail, 8);
    transform(buf_);
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        digest_[i * 4 + j] = static_cast<uint8_t>(state_[i] >> (8 * j));
      }
    }
  }

  String toString() const {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest_[i]);
    return String(hex);
  }

 private:
  static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

  void transform(const uint8_t block[64]) {
    static constexpr uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
      // memcpy rather than a cast: the block is not guaranteed aligned, and
      // the firmware this mirrors runs on a target that faults on that.
      uint32_t v;
      memcpy(&v, block + i * 4, 4);
      M[i] = v;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const uint32_t tmp = d;
      d = c;
      c = b;
      b = b + rol(a + f + K[i] + M[g], S[i]);
      a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  uint32_t state_[4]{};
  uint64_t count_ = 0;
  uint8_t buf_[72]{};  // 64 + room for the length splice in calculate()
  size_t bufLen_ = 0;
  uint8_t digest_[16];
};
