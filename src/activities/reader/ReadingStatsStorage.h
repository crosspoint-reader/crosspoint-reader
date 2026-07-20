#pragma once

#include <cstddef>
#include <cstdint>

namespace ReadingStatsStorage {

enum class ReadResult : uint8_t { Ok, Missing, TooLarge, IoError };

struct ReadOutcome {
  ReadResult result = ReadResult::Missing;
  size_t size = 0;
  uint8_t firstByte = 0;
};

// A writer must not replace a file it cannot inspect or one that belongs to a
// newer firmware. Apply this policy to both the canonical path and its backup
// before publishing, because writeAtomic() may remove or hide either one.
constexpr bool isProtectedExistingFile(const ReadResult readResult, const bool decodedAsNewerFormat) {
  return readResult == ReadResult::TooLarge || readResult == ReadResult::IoError || decodedAsNewerFormat;
}

ReadOutcome read(const char* path, uint8_t* data, size_t capacity);

// Publishes data through <path>.tmp. When rotateExisting is true, a valid old
// primary is moved to backupPath before publication. Callers leave
// rotateExisting false for corrupt primaries so a known-good backup survives.
bool writeAtomic(const char* path, const char* backupPath, bool rotateExisting, const uint8_t* data, size_t size);

}  // namespace ReadingStatsStorage
