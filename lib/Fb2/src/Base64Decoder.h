#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Streaming base64 decoder for FB2 embedded binary extraction.
// Accumulates input in a small buffer, decodes 4-char groups to 3 bytes,
// and passes decoded data to a callback. Skips whitespace automatically.
//
// Ported from papyrix-reader (https://github.com/bigbag/papyrix-reader),
// lib/Fb2/src/Base64Decoder.h.

class Base64Decoder {
 public:
  using WriteCallback = std::function<bool(const uint8_t* data, size_t len)>;

  void feed(const char* data, int len, const WriteCallback& write) {
    for (int i = 0; i < len && !failed_; i++) {
      char c = data[i];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
      buf_[bufLen_++] = c;
      if (bufLen_ == kBufSize) {
        decodeBuffer(write);
      }
    }
  }

  bool finish(const WriteCallback& write) {
    if (failed_) return false;
    if (bufLen_ > 0) {
      decodeBuffer(write);
    }
    if (bufLen_ > 0) {
      failed_ = true;  // Incomplete base64 group
    }
    return !failed_;
  }

  bool failed() const { return failed_; }

 private:
  static constexpr size_t kBufSize = 256;
  char buf_[kBufSize] = {};
  size_t bufLen_ = 0;
  bool failed_ = false;

  static int8_t decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;  // padding
    return -1;                // invalid
  }

  void decodeBuffer(const WriteCallback& write) {
    // Process complete 4-char groups
    size_t groups = bufLen_ / 4;
    size_t processed = groups * 4;
    uint8_t out[3];

    for (size_t g = 0; g < groups && !failed_; g++) {
      size_t base = g * 4;
      int8_t a = decodeChar(buf_[base]);
      int8_t b = decodeChar(buf_[base + 1]);
      int8_t c = decodeChar(buf_[base + 2]);
      int8_t d = decodeChar(buf_[base + 3]);

      if (a < 0 || b < 0) {
        failed_ = true;
        return;
      }

      out[0] = static_cast<uint8_t>((a << 2) | (b >> 4));

      if (c == -2) {
        // "xx==" — 1 output byte
        if (d != -2) {
          failed_ = true;
          return;
        }
        if (!write(out, 1)) failed_ = true;
      } else if (c < 0) {
        failed_ = true;
        return;
      } else if (d == -2) {
        // "xxx=" — 2 output bytes
        out[1] = static_cast<uint8_t>(((b & 0x0F) << 4) | (c >> 2));
        if (!write(out, 2)) failed_ = true;
      } else if (d < 0) {
        failed_ = true;
        return;
      } else {
        // "xxxx" — 3 output bytes
        out[1] = static_cast<uint8_t>(((b & 0x0F) << 4) | (c >> 2));
        out[2] = static_cast<uint8_t>(((c & 0x03) << 6) | d);
        if (!write(out, 3)) failed_ = true;
      }
    }

    // Shift remaining bytes to start
    size_t remaining = bufLen_ - processed;
    if (remaining > 0) {
      for (size_t i = 0; i < remaining; i++) {
        buf_[i] = buf_[processed + i];
      }
    }
    bufLen_ = remaining;
  }
};
