#pragma once

#include <cstddef>
#include <cstdint>

namespace ProgressFileCodec {

constexpr uint8_t TXT_MAGIC = 'T';
constexpr uint8_t TXT_VERSION = 2;
constexpr size_t TXT_V2_SIZE = 6;

enum class TxtDecodeStatus : uint8_t {
  Ok,
  LegacyPage,
  Truncated,
  WrongSize,
  BadMagic,
  UnsupportedVersion,
  NewerVersion,
};

inline uint32_t decodeU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline void encodeU32(const uint32_t value, uint8_t* data) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

inline void encodePage(const uint32_t page, uint8_t (&data)[4]) { encodeU32(page, data); }

inline uint32_t decodePage(const uint8_t (&data)[4]) { return decodeU32(data); }

inline void encodeTxtOffset(const uint32_t byteOffset, uint8_t (&data)[TXT_V2_SIZE]) {
  data[0] = TXT_MAGIC;
  data[1] = TXT_VERSION;
  encodeU32(byteOffset, data + 2);
}

// Four bytes are the legacy page number. Callers map that page through the
// freshly built page-offset table, then publish the six-byte v2 record.
inline TxtDecodeStatus decodeTxt(const uint8_t* data, const size_t size, uint32_t& value) {
  value = 0;
  if (!data || size == 0) return TxtDecodeStatus::Truncated;
  if (size == 4) {
    value = decodeU32(data);
    return TxtDecodeStatus::LegacyPage;
  }
  if (size < TXT_V2_SIZE) return TxtDecodeStatus::Truncated;
  if (size > TXT_V2_SIZE) return TxtDecodeStatus::WrongSize;
  if (data[0] != TXT_MAGIC) return TxtDecodeStatus::BadMagic;
  if (data[1] > TXT_VERSION) return TxtDecodeStatus::NewerVersion;
  if (data[1] != TXT_VERSION) return TxtDecodeStatus::UnsupportedVersion;
  value = decodeU32(data + 2);
  return TxtDecodeStatus::Ok;
}

}  // namespace ProgressFileCodec
