#pragma once

#include <cstddef>
#include <cstdint>

#include "WString.h"

namespace base64 {

namespace detail {
static const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

inline String encode(const uint8_t *data, size_t len) {
  String out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t b = (uint32_t)data[i] << 16;
    if (i + 1 < len)
      b |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len)
      b |= (uint32_t)data[i + 2];
    out += detail::kAlphabet[(b >> 18) & 0x3F];
    out += detail::kAlphabet[(b >> 12) & 0x3F];
    out += (i + 1 < len) ? detail::kAlphabet[(b >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? detail::kAlphabet[(b >> 0) & 0x3F] : '=';
  }
  return out;
}

inline String encode(const char *input) {
  return encode(reinterpret_cast<const uint8_t *>(input), strlen(input));
}

inline String encode(const String &input) {
  return encode(reinterpret_cast<const uint8_t *>(input.c_str()),
                input.length());
}

} // namespace base64
