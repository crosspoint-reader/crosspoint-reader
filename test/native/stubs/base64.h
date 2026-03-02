#pragma once
// Stub for native unit tests — identity encode/decode (no real base64)
#include <cstdint>
#include <string>

namespace base64 {

inline std::string encode(const uint8_t* data, size_t len) {
  return std::string(reinterpret_cast<const char*>(data), len);
}

}  // namespace base64
