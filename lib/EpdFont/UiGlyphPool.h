#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Bounded, shared glyph cache for SD-font UI fallback instances (the 8/10/12pt
// fonts that render CJK strings in the UI). One fixed allocation holds a sorted
// index (metrics embedded) and a byte arena of ctx3-compressed 1-bit bitmaps
// (UiGlyphCodec; raw 1-bit when a glyph doesn't compress). Entries persist
// across strings, screens, and PrewarmScopes — the point is reuse across the
// per-item draw calls UI lists make.
//
// Eviction is second-chance in insertion order over a compacted arena: the
// oldest entry is dropped unless its ref bit (set by find()) earns it a move
// to the back. Bytes stay contiguous — no fragmentation, no growth; the whole
// structure is one block, released wholesale for heap-critical transitions.
//
// Not thread-safe: callers serialize through the same render/measure paths
// that already serialize SdCardFont access.
class UiGlyphPool {
 public:
  // Glyph metrics served to the renderer; matches the EpdGlyph fields UI
  // rendering needs (bitmap addressing fields are pool-managed). left/top are
  // int8 — UI-size glyphs fit comfortably; insert() clamps. The kern class
  // IDs come from the font's kern tables (0 = glyph never kerns on that
  // side); together with the pair cache they replace the ~6.7KB resident
  // class tables per instance the arena path loads.
  struct Metrics {
    uint8_t width;
    uint8_t height;
    uint16_t advanceX;  // 12.4 fixed-point
    int8_t left;
    int8_t top;
    uint8_t kernLeftClass;
    uint8_t kernRightClass;
  };
  static_assert(sizeof(Metrics) == 8, "Metrics must stay compact");

  struct Stats {
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t inserts = 0;
    uint32_t evictions = 0;
    uint32_t rescues = 0;
    uint32_t storedBytes = 0;  // bytes currently in the arena
    uint32_t rawBytes = 0;     // 1-bit-equivalent bytes of resident glyphs
  };

  // Largest stored (encoded or raw 1-bit) bitmap; larger glyphs are rejected
  // and fall back to the caller's uncached path.
  static constexpr uint16_t MAX_ENTRY_BYTES = 160;
  // One heap-floor policy for both allocation gates: the initial init() at
  // font setup and the lazy reinit() after a heap-critical release. Keeping
  // them equal means the pool never revives into a heap it would not have
  // been created in.
  static constexpr size_t MIN_FREE_HEAP_FOR_ALLOC = 64 * 1024;
  // Scratch large enough for the biggest raw .cpfont bitmap a fill reads
  // (2-bit at MAX_ENTRY_BYTES-scale glyphs).
  static constexpr uint16_t FILL_SCRATCH_BYTES = 320;

  UiGlyphPool() = default;
  UiGlyphPool(const UiGlyphPool&) = delete;
  UiGlyphPool& operator=(const UiGlyphPool&) = delete;

  // Allocate the single block (maxEntries * 16B index + arenaBytes). Remembers
  // the geometry so reinit() can revive after release(). False on OOM.
  bool init(size_t arenaBytes, uint16_t maxEntries);
  // Re-allocate with the geometry from the last init(). No-op when ready.
  bool reinit();
  // Free the whole block (single free — cannot fragment). Entries are lost.
  void release();
  bool isReady() const { return block_ != nullptr; }
  // Drop all entries but keep the allocation.
  void reset();

  // Look up a glyph; sets its ref bit (a use, for second-chance eviction).
  // Returns an entry handle, or -1. Handles are invalidated by any insert().
  int32_t find(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint);
  // Lookup without touching ref bits or hit/miss stats — for kern queries and
  // diagnostics, which must not perturb eviction.
  int32_t peek(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint) const;
  const Metrics& metricsOf(int32_t handle) const;
  // True when the entry stores no bitmap bytes (zero-area glyphs, e.g. space).
  bool isEmptyBitmap(int32_t handle) const;
  // Decode/copy the entry's bitmap as packed 1-bit into dst. dst must hold
  // UiGlyphCodec::packed1BitBytes(width * height) bytes.
  bool copyBitmap(int32_t handle, uint8_t* dst, uint16_t dstCapacity) const;

  // Insert a glyph from its raw .cpfont bitmap (compresses, or stores raw
  // 1-bit when smaller). Replaces nothing: caller checks find() first.
  // False when the glyph is oversized or the pool cannot make room.
  bool insert(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint, const Metrics& metrics,
              const uint8_t* srcBitmap, bool srcIs2Bit);

  // Shared scratch for fill-path SD reads (one copy for all font instances).
  uint8_t* fillScratch() { return fillScratch_; }

  // Sparse kern pair cache: (instance, style, leftClass, rightClass) -> 4.4
  // fixed-point value, filled by the fonts' prewarm from the on-file matrix.
  // Zeros are cached too — 86% of matrix cells are zero and an uncached zero
  // would re-probe SD forever. Drop-when-full (UI vocabulary uses a few dozen
  // pairs); cleared by reset()/release().
  bool kernPairLookup(uint8_t instanceId, uint8_t styleIdx, uint8_t leftClass, uint8_t rightClass, int8_t* out) const;
  void kernPairInsert(uint8_t instanceId, uint8_t styleIdx, uint8_t leftClass, uint8_t rightClass, int8_t value);

  const Stats& stats() const { return stats_; }
  void logStats(const char* label) const;

 private:
  struct Entry {
    uint32_t key;     // cp(0..20) | style(21..22) | instance(23..24) | flags(30..31)
    uint16_t offset;  // byte offset into the arena
    uint16_t length;  // stored byte length (0 for empty bitmaps)
    Metrics metrics;  // 8 bytes
  };
  static_assert(sizeof(Entry) == 16, "Entry must stay compact");

  static constexpr uint32_t KEY_MASK = 0x01FFFFFFu;
  static constexpr uint32_t FLAG_ENCODED = 1u << 30;
  static constexpr uint32_t FLAG_REF = 1u << 31;

  static uint32_t makeKey(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint) {
    return (codepoint & 0x1FFFFFu) | (static_cast<uint32_t>(styleIdx & 0x3) << 21) |
           (static_cast<uint32_t>(instanceId & 0x3) << 23);
  }

  int32_t lowerBound(uint32_t key) const;
  int32_t entryAtOffset(uint16_t offset) const;  // entry whose bytes start the arena's front
  void removeEntry(int32_t idx);
  // Make room for `need` arena bytes and one index slot. May evict/rescue.
  bool makeRoom(uint16_t need);

  std::unique_ptr<uint8_t[]> block_;
  Entry* entries_ = nullptr;
  uint8_t* arena_ = nullptr;
  uint16_t entryCount_ = 0;
  uint16_t maxEntries_ = 0;
  size_t arenaBytes_ = 0;
  size_t used_ = 0;

  // Remembered geometry for reinit().
  size_t initArenaBytes_ = 0;
  uint16_t initMaxEntries_ = 0;

  static constexpr uint16_t KERN_PAIR_CAP = 256;
  static uint32_t kernPairKey(uint8_t instanceId, uint8_t styleIdx, uint8_t leftClass, uint8_t rightClass) {
    return (static_cast<uint32_t>(instanceId & 0x3) << 18) | (static_cast<uint32_t>(styleIdx & 0x3) << 16) |
           (static_cast<uint32_t>(leftClass) << 8) | rightClass;
  }
  // Sorted by key; each word is key<<8 | (uint8)value.
  uint32_t kernPairs_[KERN_PAIR_CAP] = {};
  uint16_t kernPairCount_ = 0;

  // Separate scratches: encodeScratch_ holds the pending insert's bytes while
  // makeRoom() uses entryScratch_ for rescue rotations; fillScratch_ holds the
  // caller's raw SD read (which may be the insert source).
  uint8_t entryScratch_[MAX_ENTRY_BYTES];
  uint8_t encodeScratch_[MAX_ENTRY_BYTES];
  uint8_t fillScratch_[FILL_SCRATCH_BYTES];

  Stats stats_;
};
