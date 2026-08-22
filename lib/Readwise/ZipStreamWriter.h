#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

/**
 * Minimal streaming ZIP writer using the STORE method (no compression).
 * Entries are written sequentially to an open HalFile; each local header is
 * seek-patched with its CRC/sizes on completion, so no data descriptors are
 * needed and the result opens anywhere. Sized for the small entry counts of
 * generated EPUBs — no heap allocation.
 */
class ZipStreamWriter {
 public:
  // Max entries and name length are compile-time bounds for EPUB containers.
  static constexpr uint8_t MAX_ENTRIES = 6;
  static constexpr uint8_t MAX_NAME_LEN = 63;

  bool begin(HalFile& file);
  // Starts a new stored entry; writes the local header with placeholder sizes.
  bool beginEntry(const char* name);
  // Appends raw bytes to the active entry.
  bool write(const uint8_t* data, size_t len);
  // Patches the active entry's header with its final CRC and size.
  bool finishEntry();
  // Writes the central directory + end record. The file stays open.
  bool finish();

 private:
  struct EntryInfo {
    uint32_t offset = 0;
    uint32_t crc = 0;
    uint32_t size = 0;
    char name[MAX_NAME_LEN + 1] = {0};
    uint16_t nameLen = 0;
  };

  static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

  HalFile* file_ = nullptr;
  EntryInfo entries_[MAX_ENTRIES];
  uint8_t entryCount_ = 0;
  int8_t active_ = -1;
};
