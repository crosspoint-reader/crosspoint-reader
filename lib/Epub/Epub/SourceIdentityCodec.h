#pragma once

#include <ZipFile.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SourceIdentityCodec {

constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'S', 'I'};
constexpr uint8_t VERSION = 1;
constexpr uint16_t PAYLOAD_SIZE = 26;
constexpr size_t VERSION_OFFSET = MAGIC.size();
constexpr size_t PAYLOAD_LENGTH_OFFSET = VERSION_OFFSET + 1;
constexpr size_t CRC_OFFSET = PAYLOAD_LENGTH_OFFSET + 2;
constexpr size_t PAYLOAD_OFFSET = CRC_OFFSET + 4;
constexpr size_t ENCODED_SIZE = PAYLOAD_OFFSET + PAYLOAD_SIZE;

using Encoded = std::array<uint8_t, ENCODED_SIZE>;
using Payload = std::array<uint8_t, PAYLOAD_SIZE>;

enum class DecodeStatus : uint8_t {
  OK,
  TRUNCATED,
  WRONG_SIZE,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  NEWER_VERSION,
  BAD_PAYLOAD_LENGTH,
  BAD_CRC,
  INVALID_VALUE,
};

inline uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

inline uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

inline uint64_t readU64(const uint8_t* data) {
  return static_cast<uint64_t>(readU32(data)) | (static_cast<uint64_t>(readU32(data + 4)) << 32U);
}

inline void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

inline void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

inline void writeU64(uint8_t* data, const uint64_t value) {
  writeU32(data, static_cast<uint32_t>(value));
  writeU32(data + 4, static_cast<uint32_t>(value >> 32U));
}

inline uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

inline bool valid(const ZipFile::SourceIdentity& identity) {
  const uint64_t centralDirEnd = static_cast<uint64_t>(identity.centralDirOffset) + identity.centralDirSize;
  return identity.fileSize >= 22 && identity.centralDirSize > 0 && identity.totalEntries > 0 &&
         centralDirEnd <= identity.fileSize;
}

inline bool encodePayload(const ZipFile::SourceIdentity& identity, Payload& payload) {
  if (!valid(identity)) return false;
  payload.fill(0);
  writeU64(payload.data(), identity.fileSize);
  writeU32(payload.data() + 8, identity.centralDirOffset);
  writeU32(payload.data() + 12, identity.centralDirSize);
  writeU16(payload.data() + 16, identity.totalEntries);
  writeU64(payload.data() + 18, identity.centralDirHash);
  return true;
}

inline bool decodePayload(const uint8_t* payload, const size_t length, ZipFile::SourceIdentity& identity) {
  if (!payload || length != PAYLOAD_SIZE) return false;
  ZipFile::SourceIdentity decoded;
  decoded.fileSize = readU64(payload);
  decoded.centralDirOffset = readU32(payload + 8);
  decoded.centralDirSize = readU32(payload + 12);
  decoded.totalEntries = readU16(payload + 16);
  decoded.centralDirHash = readU64(payload + 18);
  if (!valid(decoded)) return false;
  identity = decoded;
  return true;
}

inline bool encode(const ZipFile::SourceIdentity& identity, Encoded& encoded) {
  Payload payload;
  if (!encodePayload(identity, payload)) return false;
  encoded.fill(0);
  std::memcpy(encoded.data(), MAGIC.data(), MAGIC.size());
  encoded[VERSION_OFFSET] = VERSION;
  writeU16(encoded.data() + PAYLOAD_LENGTH_OFFSET, PAYLOAD_SIZE);
  writeU32(encoded.data() + CRC_OFFSET, crc32(payload.data(), payload.size()));
  std::memcpy(encoded.data() + PAYLOAD_OFFSET, payload.data(), payload.size());
  return true;
}

inline DecodeStatus decode(const uint8_t* data, const size_t length, ZipFile::SourceIdentity& identity) {
  if (!data || length < VERSION_OFFSET + 1) return DecodeStatus::TRUNCATED;
  if (std::memcmp(data, MAGIC.data(), MAGIC.size()) != 0) return DecodeStatus::BAD_MAGIC;
  if (data[VERSION_OFFSET] > VERSION) return DecodeStatus::NEWER_VERSION;
  if (data[VERSION_OFFSET] != VERSION) return DecodeStatus::UNSUPPORTED_VERSION;
  if (length < ENCODED_SIZE) return DecodeStatus::TRUNCATED;
  if (length > ENCODED_SIZE) return DecodeStatus::WRONG_SIZE;
  if (readU16(data + PAYLOAD_LENGTH_OFFSET) != PAYLOAD_SIZE) return DecodeStatus::BAD_PAYLOAD_LENGTH;
  const uint8_t* payload = data + PAYLOAD_OFFSET;
  if (readU32(data + CRC_OFFSET) != crc32(payload, PAYLOAD_SIZE)) return DecodeStatus::BAD_CRC;
  return decodePayload(payload, PAYLOAD_SIZE, identity) ? DecodeStatus::OK : DecodeStatus::INVALID_VALUE;
}

}  // namespace SourceIdentityCodec
