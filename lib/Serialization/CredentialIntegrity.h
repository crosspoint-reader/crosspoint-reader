#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace credential_integrity {

// IEEE CRC-32. This detects accidental credential corruption; it is not an
// authentication mechanism and does not make the existing XOR obfuscation
// cryptographically secure.
constexpr uint32_t crc32(const std::string_view data) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const char value : data) {
    crc ^= static_cast<uint8_t>(value);
    for (unsigned bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

constexpr bool validate(const std::string_view data, const size_t expectedLength, const uint32_t expectedCrc32) {
  return data.size() == expectedLength && crc32(data) == expectedCrc32;
}

}  // namespace credential_integrity
