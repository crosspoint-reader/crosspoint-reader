#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class BuildArena;  // lib/Memory — optional page-slot scratch, see setSlotArena

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  // Slots are keyed by EpdFontData POINTER, and every SIZE is a distinct EpdFontData -- so these
  // are shared across style x size, not "one per style" as long assumed. A page using R/B/I at
  // two sizes wants six. Exhaustion evicts the least-recently-used slot rather than refusing the
  // prewarm outright; a refused font used to send every one of its glyphs down the per-glyph
  // fallback for the whole page.
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;

  // Largest packed single glyph the per-glyph fallback cache can serve, and so the largest a
  // generated font may contain. Measured over the built-in set (45260 glyphs, 2026-08-19): the
  // largest is 513 bytes (bookerly_18_bolditalic, 54x38) and the next is 507 — so the previous
  // 512 refused exactly one glyph, by one byte, and it rendered blank on every page whose
  // prewarm missed it. 576 clears the corpus with headroom.
  //
  // Not free: this is .bss multiplied by FALLBACK_CACHE_SLOTS, i.e. 256 bytes off the heap
  // ceiling (see the sizing note there). That is why it is not simply set to something generous.
  // fontconvert.py mirrors this constant and refuses to generate a font that would overflow it,
  // so the next oversized glyph is a build error rather than an invisible blank; the host test
  // checks the shipped fonts against it from the other side.
  // 576 again since the 20 pt faces were dropped. They had raised it to 640 for U+01C4 (DZ
  // digraph) in bookerly_20_bolditalic, 60x41 -> 615 bytes; with 18 pt the largest real face, the
  // binding glyph is that digraph in bookerly_18_bolditalic again, 54x38 -> 513 bytes (measured
  // over every shipped glyph, 2026-10-18), and 576 is the next multiple of 64 above it. Synthesised
  // sizes do not move it: they fetch the 18 pt master's glyph and resample after the fetch.
  //
  // The cap is set by the widest glyph in the whole coverage, not by anything a reader meets --
  // that digraph appears in Serbo-Croatian and essentially nowhere else.
  //
  // Measured, not guessed: an earlier attempt at this constant was set from a 22 pt probe and was
  // wrong for the size that actually shipped. bench/font_main.cpp checks the binding glyph on
  // hardware, because an undersized buffer does not error -- the glyph renders BLANK.
  static constexpr uint16_t HOT_GLYPH_BUF_SIZE = 576;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first and otherwise transiently
  // allocates/decompresses the glyph's group into a temporary buffer and
  // compacts the requested glyph. The returned pointer is valid only until the
  // next getBitmap call or cache eviction; callers must copy bitmap data if a
  // longer lifetime is required.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyphRef& glyph, uint32_t glyphIndex);

  // Free all cached data (page buffers).
  void clearCache();

  // Install a scratch arena for prewarm page slots, or nullptr to go back to the heap.
  //
  // Why this exists: a page's slots are ~9 KB across six allocations, and when the prewarm runs
  // while a section build holds its working set, those six blocks land in the middle of the
  // largest free region. Measured X3 2026-08-11 — the draw cost contig 36852 -> 27636, and
  // releasing the slots afterwards returned every byte and every block while contig did not move
  // at all. The bytes were never the problem; where they sat was. Drawing them from a region the
  // build already owns keeps them off the heap entirely.
  //
  // Lifetime contract, and it is strict: arena-backed slots are NOT free()d and stop being valid
  // the moment the arena scope ends, so the caller MUST clearCache() before withdrawing the
  // arena. Use FontCacheManager::ScopedSlotArena, which does both in the right order.
  void setSlotArena(BuildArena* arena) { slotArena_ = arena; }
  bool hasArenaBackedSlots() const;

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t peakTempBytes = 0;    // largest ring held at once, in prewarm or a getBitmap miss
    uint16_t arenaTemps = 0;       // group rings served by the slot arena instead of the heap
    // Streaming stops at the last glyph a page actually wants, where the old whole-group inflate
    // always decoded to the end. These two make that saving visible: streamed/group is the
    // fraction of the group the page really paid for.
    uint32_t streamedBytes = 0;
    uint32_t groupBytes = 0;
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls

    // LRU specific stats
    uint32_t fallbackCacheHits = 0;
    uint32_t fallbackCacheMisses = 0;

    // Fallback-path OOM latch. Smallest group size (bytes) whose transient allocation
    // failed during this pass, or 0 if none has. Lives in Stats so it is cleared by
    // resetStats() — i.e. it lasts exactly one render phase, and the next phase retries
    // with whatever heap is available by then. See getBitmap().
    uint32_t fallbackOomBytes = 0;
    uint16_t fallbackOomGlyphs = 0;  // glyphs that got no bitmap because of it
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  BuildArena* slotArena_ = nullptr;  // not owned; see setSlotArena

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
    uint16_t groupIndex;     // cached to avoid re-calling getGroupIndex in prewarm Step 4
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
    uint32_t bufferBytes = 0;   // size of `buffer`, so eviction can unwind the stats it added
    uint32_t lastUsedTick = 0;  // bumped on prewarm and on every getBitmap hit; drives eviction
    // Buffer/glyphs came from the scratch arena, not the heap: they must NOT be free()d, and
    // they die when the arena scope ends (see setSlotArena).
    bool arenaBacked = false;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;
  uint32_t pageSlotTick_ = 0;

  // A 1-slot LRU is pathological: consecutive distinct glyphs evict each other every time, so
  // the fallback degenerates to a full group inflate per glyph. Measured X3 on a page whose
  // prewarm missed its heading size: 16 getBitmap calls, 121007 us total (7562 us/call),
  // fallback hits=1 misses=12.
  //
  // Sized 4, not 8. These slots live in .bss, so each one is permanently off the heap CEILING
  // rather than merely occupying heap: going to 8 measurably dropped total heap from 266864 to
  // 263152 (7 x ~530 B), and on a device already reading at ~30 KB free that is not free.
  // 4 keeps the 4x reduction in eviction thrash for a third of the standing cost.
  static constexpr uint8_t FALLBACK_CACHE_SLOTS = 4;

  struct FallbackSlot {
    const EpdFontData* fontData;
    uint32_t glyphIndex;
    uint32_t lastUsedTick;
    uint8_t buffer[HOT_GLYPH_BUF_SIZE];
  };

  // LRU cache for fallback glyph decompresion
  FallbackSlot _fallbackCache[FALLBACK_CACHE_SLOTS] = {};
  uint32_t _fallbackTick = 0;

  void freePageBuffer();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);

 public:
  // A group being read one glyph at a time, without ever holding the whole group.
  //
  // The group's DEFLATE stream is decoded through a caller-supplied ring of
  // EpdFontGroup::ringBytes (the largest back-reference the encoder emitted), and each glyph
  // is compacted out of the passing byte stream. So the transient cost of reaching a glyph is
  // the ring, not the group's uncompressed size, and a group may grow as large as compression
  // wants without costing the reader a byte more RAM.
  //
  // Glyphs must be requested in ascending alignedOffset — a stream cannot rewind. Within one
  // group that is the same as ascending glyph index (the aligned layout follows glyph order),
  // which is the order both call sites already iterate in. A backwards request is refused
  // rather than silently mis-decoded.
  //
  // Public only so the host test can exercise it directly; nothing outside this class and its
  // test constructs one.
  class GroupStream {
   public:
    // `ring` must be at least ringBytesFor(group) bytes and stay alive for the whole stream.
    bool begin(const EpdFontData* fontData, const EpdFontGroup& group, uint8_t* ring, uint32_t ringBytes);

    // Compact one glyph from the stream into `packedDst` (glyph.dataLength bytes).
    // Zero-size glyphs consume nothing and succeed.
    bool extractGlyph(uint32_t alignedOffset, const EpdGlyphRef& glyph, uint8_t* packedDst);

    // Bytes of the group decoded so far — the CPU actually spent, for stats.
    uint32_t consumed() const { return pos_; }

   private:
    // Discard forward to `offset`, which is where the early-out lives: the stream is never
    // read past the last glyph the page asked for.
    bool skipTo(uint32_t offset);
    bool readExact(uint8_t* dest, uint32_t len);

    InflateReader reader_;
    uint32_t pos_ = 0;    // absolute position in the decompressed group
    uint32_t limit_ = 0;  // group.uncompressedSize; reads past it are a stream error
  };

  // RAM a group costs to read: its measured ring, or the whole group for fonts generated
  // before ringBytes existed. Never more than the group, since a back-reference cannot
  // outrun the bytes produced so far.
  static uint32_t ringBytesFor(const EpdFontGroup& group);

 private:
  // Widest byte-aligned row any glyph can have: width is a uint8_t, and a byte-aligned row is
  // ceil(width/4) bytes, so 64 is a hard bound rather than a corpus measurement. Small enough
  // that both the compaction row buffer and the skip sink live on the stack.
  static constexpr uint16_t MAX_ROW_STRIDE = 64;
};
