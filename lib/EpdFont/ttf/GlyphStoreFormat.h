#pragma once

#include <cstddef>
#include <cstdint>

/// On-flash format of the TTF glyph store in the `spiffs` partition (magic "CPTT").
///
///   0            header sector: Header, written once per install
///   4 KB         state sector: the face table, then the wipe word; a wipe erases it last
///   64 KB        style blocks, each starting on a 64 KB page so one maps alone (flash mode: the whole
///                TTF; SD mode: the tables copied out of it), every family's styles in the set's order
///   indexOffset  glyph index: Slot {key, the record's dataOffset, or kNoGlyph}, open addressing
///   pairOffset   kerning pair index: Slot {left << 16 | right, pairStyle(family, style) << 16 | font units}
///   logOffset    append-only glyph records: [u32 key][EpdGlyph][2-bit bitmap], each 4-byte aligned
///
/// Every byte is programmed at most once between erases. Erased flash reads 0xFF, so "erased" doubles as
/// "empty". A power cut can only leave bytes with more 1 bits than were written (NorFlash.h); every marker
/// that must not appear by accident is therefore one that only a complete write produces.
///
/// The store holds a family set (spec 11.1): one family, or a book's body family and its accent family. A family is
/// its index in the set. The set's identity covers their order, so an index names the same family for as long as
/// the store stays installed.
namespace glyphstore {

constexpr uint32_t kMagic = 0x54545043;  // "CPTT"
/// 1: one family, and a size table of ppems.
/// 2: a family set (Stage B): the size table became a face table (faceOf()), a pair slot carries the family beside
/// the style, and the header has eight style blocks. A version-1 store reads as not installed and is installed over.
constexpr uint16_t kFormatVersion = 2;
constexpr uint8_t kModeFlash = 0;  ///< every style's whole TTF is in the partition
constexpr uint8_t kModeSd = 1;     ///< the tables are in the partition; outlines are read from the SD card
/// Style blocks of every family together: two families of four, or a body of four and four one-style accent
/// families. A family has at least its regular style, so a set has at most this many families.
constexpr uint8_t kMaxStyles = 8;
constexpr uint8_t kMaxTables = 8;
/// Faces (a family at one size) claimed since the last wipe, every family's: a book's body takes 5-10 sizes and an
/// accent family 2-4.
constexpr uint8_t kFaceEntries = 32;
constexpr uint32_t kIdentityBytes = 16;

constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kPageBytes = 65536;  ///< one MMU page: mappings are paid in whole pages
constexpr uint32_t kStateSectorOffset = kSectorBytes;
constexpr uint32_t kFirstRegionOffset = 2 * kSectorBytes;  ///< nothing else may start before this
constexpr uint32_t kHeaderRegionBytes = kPageBytes;        ///< planLayout starts the style blocks a page in

constexpr uint32_t kErased = 0xFFFFFFFF;
constexpr uint32_t kNoGlyph = 0;  ///< slot value: the font has no glyph for this key; a cut can't produce 0
constexpr uint32_t kSlotBytes = 8;
constexpr uint32_t kSlotsPerPage = kPageBytes / kSlotBytes;  // 8,192
constexpr uint32_t kMaxProbe = 128;         ///< a longer chain counts as full; tables have at least this many slots
constexpr uint32_t kIndexLoadPercent = 70;  ///< more slots in use than this counts as full
constexpr uint32_t kLogFullPercent = 95;

constexpr uint32_t kRecordHeaderBytes = 20;  ///< u32 key + 16-byte EpdGlyph
constexpr uint32_t kMaxBitmapBytes = 8192;
constexpr uint32_t kMaxRecordBytes = kRecordHeaderBytes + kMaxBitmapBytes;

constexpr uint32_t kMinLogPages = 4;
constexpr uint32_t kPairPages = 1;
constexpr uint32_t kSparePages = 2;
/// Expected bytes per stored record, used only to size the index against the log: a Latin glyph at
/// 14 pt is about 100 B, a CJK glyph about 250 B (measured on the X3).
constexpr uint32_t kExpectedRecordFlash = 100;
constexpr uint32_t kExpectedRecordSd = 250;

/// Glyph key: code point (21 bits), style (2), face-table index (5), which says the family and the size. Bits
/// 28-31 stay zero, so a key is never the erased value.
constexpr uint32_t makeKey(const uint32_t codepoint, const uint8_t style, const uint8_t faceIndex) {
  return (codepoint & 0x1FFFFFu) | (static_cast<uint32_t>(style & 0x3u) << 21) |
         (static_cast<uint32_t>(faceIndex & 0x1Fu) << 23);
}
constexpr bool isGlyphKey(const uint32_t key) { return (key >> 28) == 0; }
constexpr uint8_t keyFaceIndex(const uint32_t key) { return static_cast<uint8_t>((key >> 23) & 0x1Fu); }
/// A face-table entry's face: the family in the top byte, the pixels per em (26.6, under 2^14 at 120 px) below it.
/// A family is below kMaxStyles, so a face never reads as erased.
constexpr uint32_t faceOf(const uint8_t family, const uint32_t ppem26_6) {
  return (static_cast<uint32_t>(family) << 24) | (ppem26_6 & 0xFFFFFFu);
}
constexpr uint32_t pairKey(const uint16_t left, const uint16_t right) {
  return (static_cast<uint32_t>(left) << 16) | right;
}
/// A pair slot's value holds this in its high half: kerning is a family's and a style's, the same at every size.
constexpr uint32_t pairStyle(const uint8_t family, const uint8_t style) {
  return (static_cast<uint32_t>(family) << 2) | (style & 0x3u);
}

/// Home slot of `key` in a table of `slots`: Fibonacci hashing, then a multiply-shift into the range. For
/// a power-of-two table this is `(key * 2654435761) >> (32 - log2(slots))`; it also serves any slot count,
/// so the index can be sized in whole pages.
constexpr uint32_t homeSlot(const uint32_t key, const uint32_t slots) {
  return static_cast<uint32_t>((static_cast<uint64_t>(key * 2654435761u) * slots) >> 32);
}

constexpr uint32_t align4(const uint32_t n) { return (n + 3u) & ~3u; }
/// Bytes a record with a `dataLength`-byte bitmap takes in the log, and in a GlyphBatch.
constexpr uint32_t recordBytes(const uint32_t dataLength) { return align4(kRecordHeaderBytes + dataLength); }
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

/// Bump kFormatVersion whenever this layout changes.
struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;              ///< sizeof(Header) when written
  uint8_t identity[kIdentityBytes];  ///< the family set's identity, compared at open
  Geometry geometry;
  StyleInfo styles[kMaxStyles];
  uint32_t check;  ///< FNV-1a over every byte before it: a torn header write fails it
};
static_assert(sizeof(Header) <= kSectorBytes, "the header fits its sector");

/// A face-table entry. In use once `claimed` reads zero. The face is written first and `claimed` last, so
/// an entry cut short reads as neither free nor in use, and is never claimed again.
struct FaceEntry {
  uint32_t face;  ///< faceOf(family, ppem)
  uint32_t claimed;
};
static_assert(sizeof(FaceEntry) == 8, "a face entry is 8 bytes");

constexpr uint32_t kFaceTableOffset = kStateSectorOffset;
constexpr uint32_t kWipeWordOffset = kFaceTableOffset + kFaceEntries * sizeof(FaceEntry);
static_assert(kFaceEntries <= 32, "claimed faces are tracked in a 32-bit mask");

inline uint32_t fnv1a(const void* data, const size_t len, uint32_t hash = 2166136261u) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

/// What the caller knows before an install: the mode, and per style block the bytes to copy (flash mode: the
/// TTF file; SD mode: the copied tables, summed), every family's styles in the set's order.
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

/// Region sizes from the mapping budget:
///   log pages = free pages - mapped style pages - index pages - pair pages - 2 spare,
/// also bounded by the partition. The index gets enough pages that 70 % of its slots holds the
/// records the log holds (kExpectedRecord*), so it does not fill first. Flash mode maps one style at a
/// time, so the largest style counts; SD mode maps all copied tables together. False when the log
/// would be under kMinLogPages.
bool planLayout(const PlanInput& in, Layout& out);

}  // namespace glyphstore
