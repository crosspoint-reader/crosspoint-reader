#include "ReadingStatsEnvelope.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'S', 'E'};

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
         static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}
}  // namespace

namespace ReadingStatsEnvelope {

uint32_t crc32(const uint8_t* data, const size_t size) {
  if (!data && size != 0) return 0;
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

size_t encode(const Kind kind, const uint8_t* payload, const size_t payloadSize, Bytes& encoded) {
  encoded.fill(0);
  if (!payload || payloadSize == 0 || payloadSize > MAX_PAYLOAD_SIZE) return 0;
  std::copy(MAGIC.begin(), MAGIC.end(), encoded.begin());
  encoded[4] = CURRENT_VERSION;
  encoded[5] = static_cast<uint8_t>(kind);
  writeLe16(encoded.data(), 6, static_cast<uint16_t>(payloadSize));
  memcpy(encoded.data() + HEADER_SIZE, payload, payloadSize);
  const size_t crcOffset = HEADER_SIZE + payloadSize;
  writeLe32(encoded.data(), crcOffset, crc32(encoded.data(), crcOffset));
  return crcOffset + CRC_SIZE;
}

DecodeResult decode(const uint8_t* encoded, const size_t encodedSize, const Kind expectedKind, const uint8_t*& payload,
                    size_t& payloadSize) {
  payload = nullptr;
  payloadSize = 0;
  if (!encoded || encodedSize < HEADER_SIZE + CRC_SIZE || !std::equal(MAGIC.begin(), MAGIC.end(), encoded)) {
    return DecodeResult::Invalid;
  }
  if (encoded[4] > CURRENT_VERSION) return DecodeResult::NewerFormat;
  if (encoded[4] != CURRENT_VERSION) return DecodeResult::Invalid;

  const size_t decodedPayloadSize = readLe16(encoded, 6);
  if (decodedPayloadSize == 0 || decodedPayloadSize > MAX_PAYLOAD_SIZE ||
      encodedSize != HEADER_SIZE + decodedPayloadSize + CRC_SIZE) {
    return DecodeResult::Invalid;
  }
  const size_t crcOffset = HEADER_SIZE + decodedPayloadSize;
  if (readLe32(encoded, crcOffset) != crc32(encoded, crcOffset)) return DecodeResult::Invalid;
  if (encoded[5] != static_cast<uint8_t>(expectedKind)) return DecodeResult::WrongKind;

  payload = encoded + HEADER_SIZE;
  payloadSize = decodedPayloadSize;
  return DecodeResult::Ok;
}

ReadOutcome read(const char* path, const Kind expectedKind, uint8_t* payload, const size_t payloadCapacity) {
  ReadOutcome outcome;
  Bytes encoded{};
  const ReadingStatsStorage::ReadOutcome stored = ReadingStatsStorage::read(path, encoded.data(), encoded.size());
  outcome.readResult = stored.result;
  if (stored.result != ReadingStatsStorage::ReadResult::Ok) return outcome;

  const uint8_t* decodedPayload = nullptr;
  size_t decodedPayloadSize = 0;
  outcome.decodeResult = decode(encoded.data(), stored.size, expectedKind, decodedPayload, decodedPayloadSize);
  if (outcome.decodeResult != DecodeResult::Ok) return outcome;
  // A valid envelope whose payload is larger than this firmware understands
  // may belong to a future payload revision. Keep it distinct from corruption
  // so callers protect it from fallback, reset and overwrite.
  if (decodedPayloadSize > payloadCapacity) {
    outcome.decodeResult = DecodeResult::PayloadTooLarge;
    return outcome;
  }
  if (!payload) {
    outcome.decodeResult = DecodeResult::Invalid;
    return outcome;
  }
  memcpy(payload, decodedPayload, decodedPayloadSize);
  outcome.payloadSize = decodedPayloadSize;
  return outcome;
}

bool writeAtomic(const char* path, const char* backupPath, const bool rotateExisting, const Kind kind,
                 const uint8_t* payload, const size_t payloadSize) {
  Bytes encoded{};
  const size_t encodedSize = encode(kind, payload, payloadSize, encoded);
  if (encodedSize == 0 ||
      !ReadingStatsStorage::writeAtomic(path, backupPath, rotateExisting, encoded.data(), encodedSize)) {
    return false;
  }

  std::array<uint8_t, MAX_PAYLOAD_SIZE> verified{};
  const ReadOutcome readback = read(path, kind, verified.data(), verified.size());
  return readback.readResult == ReadingStatsStorage::ReadResult::Ok && readback.decodeResult == DecodeResult::Ok &&
         readback.payloadSize == payloadSize && memcmp(verified.data(), payload, payloadSize) == 0;
}

}  // namespace ReadingStatsEnvelope
