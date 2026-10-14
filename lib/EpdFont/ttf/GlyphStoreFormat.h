#pragma once

#include <cstddef>
#include <cstdint>

/// On-flash format of the TTF glyph store in the `spiffs` partition (magic "CPTT"). Spec:
/// docs/superpowers/specs/2026-10-13-ttf-fonts-design.md, section 4.
///
///   0            header sector (4 KB): Header, then the size table (0x400) and the wipe word (0x480)
///   64 KB        style blocks, each starting on a 64 KB page so one maps alone (flash mode: the whole
///                TTF; SD mode: the tables copied out of it)
///   indexOffset  glyph index: Slot {key, log offset}, open addressing
///   pairOffset   kerning pair index: Slot {left << 16 | right, style << 16 | font units}
///   logOffset    append-only glyph records: [u32 key][EpdGlyph][2-bit bitmap], each 4-byte aligned
///
/// Outside the header sector every byte is programmed at most once between erases. Erased flash reads
/// 0xFF, so "erased" doubles as "empty".
namespace glyphstore {

constexpr uint32_t kMagic = 0x54545043;  // "CPTT"
constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kModeFlash = 0;  ///< every style's whole TTF is in the partition
constexpr uint8_t kModeSd = 1;     ///< the tables are in the partition; outlines are read from the SD card
constexpr uint8_t kMaxStyles = 4;
constexpr uint8_t kMaxTables = 8;
constexpr uint8_t kSizeEntries = 16;

constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kPageBytes = 65536;               ///< one MMU page: mappings are paid in whole pages
constexpr uint32_t kHeaderRegionBytes = kPageBytes;  ///< sector 0 is the header; the regions start a page later
constexpr uint32_t kSizeTableOffset = 0x400;
constexpr uint32_t kWipeWordOffset = 0x480;

constexpr uint32_t kErased = 0xFFFFFFFF;
constexpr uint32_t kMissing = 0xFFFFFFFE;  ///< slot value: the font has no glyph for this key; no record
constexpr uint32_t kSlotBytes = 8;
constexpr uint32_t kSlotsPerPage = kPageBytes / kSlotBytes;  // 8,192
constexpr uint32_t kMaxProbe = 32;
constexpr uint32_t kIndexLoadPercent = 70;  ///< more slots in use than this counts as full (4.6)
constexpr uint32_t kLogFullPercent = 95;

constexpr uint32_t kRecordHeaderBytes = 20;  ///< u32 key + 16-byte EpdGlyph
constexpr uint32_t kMaxBitmapBytes = 8192;
constexpr uint32_t kMaxRecordBytes = kRecordHeaderBytes + kMaxBitmapBytes;

constexpr uint32_t kMinLogPages = 4;
constexpr uint32_t kPairPages = 1;
constexpr uint32_t kSparePages = 2;
/// Expected bytes per stored record, used only to size the index against the log: a Latin glyph at
/// 14 pt is about 100 B, a CJK glyph about 250 B (Appendix A).
constexpr uint32_t kExpectedRecordFlash = 100;
constexpr uint32_t kExpectedRecordSd = 250;

/// Glyph key: code point (21 bits), style (2), size-table index (4). Bits 27-31 stay zero, so a key is
/// never the erased value.
constexpr uint32_t makeKey(const uint32_t codepoint, const uint8_t style, const uint8_t sizeIndex) {
  return (codepoint & 0x1FFFFFu) | (static_cast<uint32_t>(style & 0x3u) << 21) |
         (static_cast<uint32_t>(sizeIndex & 0xFu) << 23);
}
constexpr bool isGlyphKey(const uint32_t key) { return (key >> 27) == 0; }
constexpr uint32_t pairKey(const uint16_t left, const uint16_t right) {
  return (static_cast<uint32_t>(left) << 16) | right;
}

/// Home slot of `key` in a table of `slots`: Fibonacci hashing, then a multiply-shift into the range. For
/// a power-of-two table this is the spec's `(key * 2654435761) >> (32 - log2(slots))`; it also serves any
/// slot count, so the index can be sized in whole pages.
constexpr uint32_t homeSlot(const uint32_t key, const uint32_t slots) {
  return static_cast<uint32_t>((static_cast<uint64_t>(key * 2654435761u) * slots) >> 32);
}

constexpr uint32_t align4(const uint32_t n) { return (n + 3u) & ~3u; }
constexpr uint32_t alignUp(const uint32_t n, const uint32_t to) { return (n + to - 1) / to * to; }
constexpr uint32_t pagesFor(const uint32_t bytes) { return (bytes + kPageBytes - 1) / kPageBytes; }

struct Slot {
  uint32_t key;
  uint32_t value;
};
static_assert(sizeof(Slot) == kSlotBytes, "a slot is 8 bytes");

/// SD mode: where a table copied out of the TTF sits.
struct TableEntry {
  uint32_t tag;          ///< the OpenType tag as read from the file
  uint32_t fileOffset;   ///< offset in the TTF file
  uint32_t blockOffset;  ///< offset in this style's block
  uint32_t length;
};

struct StyleInfo {
  uint32_t offset;      ///< partition offset of the style block
  uint32_t size;        ///< bytes of style data in the block
  uint32_t sdPathHash;  ///< SD mode: the TTF file the outlines come from
  uint32_t fileSize;    ///< SD mode
  uint32_t tableCount;  ///< SD mode: entries used in tables
  TableEntry tables[kMaxTables];
};

struct Geometry {
  uint8_t mode;
  uint8_t styleCount;
  uint16_t reserved;
  uint32_t indexOffset;
  uint32_t indexSlots;
  uint32_t pairOffset;
  uint32_t pairSlots;
  uint32_t logOffset;
  uint32_t logSize;
};

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;  ///< sizeof(Header) when written
  uint8_t identity[16];  ///< the family identity (4.2), compared at open
  Geometry geometry;
  StyleInfo styles[kMaxStyles];
  uint32_t check;  ///< FNV-1a over every byte before it: a torn header write fails it
};
static_assert(sizeof(Header) <= kSizeTableOffset, "the header overlaps the size table");

/// A size-table entry. It is in use once `pad` reads zero: the pad is written last, so an entry cut short
/// reads as neither free nor in use, and is never claimed again.
struct SizeEntry {
  uint32_t ppem26_6;
  uint8_t flags;
  uint8_t pad[3];
};
static_assert(sizeof(SizeEntry) == 8, "a size entry is 8 bytes");
static_assert(kSizeTableOffset + kSizeEntries * sizeof(SizeEntry) <= kWipeWordOffset,
              "the size table overlaps the wipe word");

inline uint32_t fnv1a(const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

/// What the caller knows before an install (7.1): the mode, and per style the bytes to copy (flash
/// mode: the TTF file; SD mode: the copied tables, summed).
struct PlanInput {
  uint8_t mode;
  uint8_t styleCount;
  uint32_t styleBytes[kMaxStyles];
  uint32_t usableBytes;   ///< NorFlash::size(): the partition below the language slot
  uint32_t freeMapPages;  ///< NorFlash::freeMapPages(), measured while nothing of the store is mapped
};

struct Layout {
  Geometry geometry;
  uint32_t styleOffset[kMaxStyles];
  uint32_t styleSize[kMaxStyles];
};

/// Region sizes from the mapping budget (4.1):
///   log pages = free pages - mapped style pages - index pages - pair pages - 2 spare,
/// also bounded by the partition. The index gets enough pages that 70 % of its slots holds the
/// records the log holds (kExpectedRecord*), so it does not fill first. Flash mode maps one style at a
/// time, so the largest style counts; SD mode maps all copied tables together. False when the log
/// would be under kMinLogPages.
bool planLayout(const PlanInput& in, Layout& out);

}  // namespace glyphstore
