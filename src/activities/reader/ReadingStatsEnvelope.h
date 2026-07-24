#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsStorage.h"

namespace ReadingStatsEnvelope {

// CrossVi keeps the CrossInk payload layouts for compatibility, but wraps
// every canonical on-disk snapshot so torn writes and silent bit flips are
// distinguishable from valid zero-valued statistics.
enum class Kind : uint8_t { Book = 1, Global = 2, PeerGlobal = 3, DailyGlobal = 4 };
enum class DecodeResult : uint8_t { Ok, Invalid, NewerFormat, WrongKind, PayloadTooLarge };

constexpr uint8_t CURRENT_VERSION = 1;
constexpr size_t HEADER_SIZE = 8;
constexpr size_t CRC_SIZE = 4;
constexpr size_t MAX_PAYLOAD_SIZE = 159;
constexpr size_t MAX_FILE_SIZE = HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE;
using Bytes = std::array<uint8_t, MAX_FILE_SIZE>;

struct ReadOutcome {
  ReadingStatsStorage::ReadResult readResult = ReadingStatsStorage::ReadResult::Missing;
  DecodeResult decodeResult = DecodeResult::Invalid;
  size_t payloadSize = 0;
};

uint32_t crc32(const uint8_t* data, size_t size);
size_t encode(Kind kind, const uint8_t* payload, size_t payloadSize, Bytes& encoded);
DecodeResult decode(const uint8_t* encoded, size_t encodedSize, Kind expectedKind, const uint8_t*& payload,
                    size_t& payloadSize);
ReadOutcome read(const char* path, Kind expectedKind, uint8_t* payload, size_t payloadCapacity);

// ReadingStatsStorage performs temp write, sync, byte verification, rename and
// final byte verification. This wrapper additionally decodes the published
// file and rechecks its CRC and payload before reporting success.
bool writeAtomic(const char* path, const char* backupPath, bool rotateExisting, Kind kind, const uint8_t* payload,
                 size_t payloadSize);

}  // namespace ReadingStatsEnvelope
