#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Multi-entry flash font cache.
//
// The unused `spiffs` partition (3.47 MB at 0xc90000) stores one or more
// .cpfont files so font metadata can be mmap'd directly from flash — zero SRAM
// cost for fullIntervals / kern / ligature tables during layout.
//
// The TAIL of the partition is NOT ours: the last LANG_RESERVED_BYTES belong to
// FlashLangPartition, which keeps the decompressed UI language there. See
// fontUsableSize(). Two consequences, both handled here rather than at the call
// sites:
//   * beginWrite() erases and budgets only up to the ceiling. It used to erase
//     part->size, which would wipe the language slot every time a font family
//     was cached. Lowering WriteSession::partitionSize is what makes
//     appendFile()'s existing overflow check honour the reservation too — that
//     covers SdCardFontManager's single-file fallback path, which does not
//     apply its own PARTITION_CAP.
//   * readIndex() rejects an index whose data runs past the ceiling. That is
//     the migration case: a partition written by firmware from before the
//     reservation existed may hold font data where the language now lives.
//     Treating it as invalid re-derives the cache from the .cpfont files on the
//     SD card, which are the originals — the flash copy is only ever a cache.
//
// Partition layout:
//   [4 B]  magic  "CPFC"
//   [1 B]  entry count N  (1..MAX_ENTRIES)
//   [3 B]  reserved (zero)
//   [N × ENTRY_SIZE bytes]  index entries (see Entry struct below)
//   [font data, packed sequentially, each entry 4-byte aligned]
//
// Each index entry:
//   [32 B]  family name, null-padded
//   [ 1 B]  point size
//   [ 3 B]  padding
//   [ 4 B]  data offset from start of partition (uint32_t LE)
//   [ 4 B]  data size in bytes (uint32_t LE)
//   [ 4 B]  reserved
//   = 48 bytes per entry
//
// Typical usage (whole family cached, e.g. Bitter 5 sizes ≈ 2.3 MB):
//   FlashFontPartition::beginWrite("Bitter");
//   for each size: FlashFontPartition::appendFile(sdPath, "Bitter", ptSize);
//   FlashFontPartition::finaliseWrite();
//
//   // To load:
//   FlashFontPartition::mmap("Bitter", 16, &ptr, &sz);
//   SdCardFont::loadFromMmap(ptr, sz, sdPath);
//
// CJK / oversized fonts that don't fit as a family fall back to single-entry:
//   FlashFontPartition::beginWrite("NotoSansCJK");
//   FlashFontPartition::appendFile(sdPath, "NotoSansCJK", 10);
//   FlashFontPartition::finaliseWrite();

namespace FlashFontPartition {

static constexpr uint8_t MAX_ENTRIES = 16;  // enough for 3 families × 5 sizes + margin
static constexpr size_t ENTRY_SIZE = 48;
static constexpr size_t HEADER_BYTES = 8 + MAX_ENTRIES * ENTRY_SIZE;  // 776 bytes

// Bytes at the END of the partition reserved for FlashLangPartition.
//
// 64 KB because esp_partition_mmap() aligns to 64 KB and the language slot has
// to start on such a boundary: the partition begins at 0xc90000 and is an exact
// multiple of 64 KB, so size - 64 KB is aligned. The largest language needs
// ~31 KB (header + offset table + blob), so half the slot is spare.
//
// This costs the font cache nothing in practice. SdCardFontManager already
// declines to use the top of the partition — its PARTITION_CAP is 3300 KB
// against a real 3,538,944 B — so 159,744 B were already going unused and this
// reservation fits inside that with ~92 KB still spare.
static constexpr size_t LANG_RESERVED_BYTES = 64 * 1024;

// Bytes of the partition the font cache may use: everything below the language
// slot. Returns 0 if the partition is missing or smaller than the reservation.
size_t fontUsableSize();

struct Entry {
  char familyName[32];
  uint8_t pointSize;
  uint8_t pad[3];
  uint32_t dataOffset;  // byte offset from partition start
  uint32_t dataSize;
  uint32_t reserved;
};
static_assert(sizeof(Entry) == ENTRY_SIZE, "Entry size mismatch");

// --- Write API ---

// Start a new write session for a named family. Clears any previous partition
// content. Must be called before appendFile(). Returns false if partition not found.
bool beginWrite(const char* familyName);

// Append one .cpfont file to the current write session.
// familyName and pointSize are recorded in the index entry.
// Returns false on SD read error or if the file would overflow the partition.
bool appendFile(const char* sdPath, const char* familyName, uint8_t pointSize);

// Commit the index header to flash and finish the write session.
// Returns false if no files were appended or if the write fails.
bool finaliseWrite();

// --- Read API ---

// Memory-map the entry for (familyName, pointSize).
// outPtr receives a pointer to the start of the .cpfont data; outSize receives
// its byte count. Returns false if no matching entry exists or mmap fails.
// The map is valid until unmap() is called.
bool mmap(const char* familyName, uint8_t pointSize, const uint8_t** outPtr, size_t* outSize);

// Release the current mmap. No-op if not mapped.
void unmap();

// --- Query API ---

// Returns true if the partition contains a valid index with at least one entry.
bool hasValidIndex();

// Returns true if the partition has an entry for (familyName, pointSize).
bool hasEntry(const char* familyName, uint8_t pointSize);

// Returns true if the partition has all sizes in the provided array.
bool hasFamilyComplete(const char* familyName, const uint8_t* sizes, uint8_t sizeCount);

// Returns true if the partition is currently mmap'd.
bool isMapped();

// Returns true while a write session (beginWrite .. finaliseWrite) is open.
bool isWriting();

}  // namespace FlashFontPartition
