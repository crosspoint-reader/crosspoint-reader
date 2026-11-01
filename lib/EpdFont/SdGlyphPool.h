#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdFontData.h"

class SdGlyphPool {
 public:
  static constexpr uint16_t CHUNK_BYTES = 4096;
  static constexpr uint8_t INTERNAL_CHUNKS = 9;
  static constexpr uint8_t PSRAM_CHUNKS = 32;
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  // Below this internal free heap, a cache boundary frees every unpinned chunk.
  static constexpr size_t RETAIN_MIN_FREE_HEAP = 40 * 1024;
  static constexpr uint8_t NO_FONT = 255;
  struct Entry {
    uint32_t codepoint;
    EpdGlyph glyph;
    uint16_t generation;
    uint8_t fontStyle;
    bool bitmapReady;
  };
  static_assert(sizeof(Entry) == 24);
  static_assert(PSRAM_CHUNKS * (CHUNK_BYTES / 4) < UINT16_MAX);
  struct Scratch {
    uint32_t* codepoints = nullptr;
    Entry** pending = nullptr;
    uint16_t capacity = 0;
  };

  SdGlyphPool();
  ~SdGlyphPool();
  SdGlyphPool(const SdGlyphPool&) = delete;
  SdGlyphPool& operator=(const SdGlyphPool&) = delete;
  uint8_t registerFont();
  void unregisterFont(uint8_t slot);
  void dropFont(uint8_t slot);
  void release();
  // Cache boundary: new pin generation, overflow freed, chunks trimmed on a tight heap.
  void clearCache();
  // Frees every chunk without a current-generation entry, for callers that need a large
  // block; true when it freed at least one.
  bool trimUnpinned();
  void beginScope();
  void endScope();
  void beginPrewarm();
  Entry* find(uint8_t fontStyle, uint32_t codepoint);
  Entry* insert(uint8_t fontStyle, uint32_t codepoint, const EpdGlyph& glyph, bool prewarm = false);
  Entry* insertOverflow(uint8_t fontStyle, uint32_t codepoint, const EpdGlyph& glyph);
  bool isOverflow(const Entry* entry) const;
  Scratch* scratch(uint16_t needed = 0);
  static uint8_t* bitmap(Entry* entry) { return reinterpret_cast<uint8_t*>(entry + 1); }
  static uint32_t entryBytes(const EpdGlyph& glyph) { return (sizeof(Entry) + glyph.dataLength + 3) & ~3u; }

 private:
  static constexpr uint16_t NO_ENTRY = UINT16_MAX;
  static constexpr uint32_t DROPPED = UINT32_MAX;
  // Dense CJK pages overflow a few glyphs each; an evicted overflow glyph is re-read on every render pass.
  static constexpr uint8_t OVERFLOW_ENTRIES = 8;
  struct Chunk {
    uint8_t* data = nullptr;
    uint16_t used = 0;
  };
  Chunk chunks_[PSRAM_CHUNKS];
  uint8_t order_[PSRAM_CHUNKS] = {};
  uint8_t chunkCount_ = 0;
  uint8_t chunkCap_ = 0;
  uint16_t generation_ = 1;
  uint16_t scopeDepth_ = 0;
  uint64_t fonts_ = 0;
  uint16_t* index_ = nullptr;
  uint16_t indexCapacity_ = 0;
  uint16_t entryCap_ = 0;
  uint16_t entryCount_ = 0;
  Scratch scratch_;
  uint16_t scratchHint_ = 0;  // capacity before the last release, so a tight heap does not regrow in steps
  uint8_t* overflow_ = nullptr;
  uint32_t overflowCapacity_ = 0;
  uint32_t overflowHead_ = 0;
  uint32_t overflowOffsets_[OVERFLOW_ENTRIES] = {};
  uint8_t overflowCount_ = 0;
  Entry* entry(uint16_t encoded) const;
  uint16_t indexSlot(uint8_t fontStyle, uint32_t codepoint) const;
  void rebuildIndex();
  bool pinned(uint8_t chunk) const;
  void advanceGeneration();
  void makeNewest(uint8_t orderIndex, uint16_t used = 0);
  bool compactForPrewarm(uint8_t orderIndex, uint32_t bytes);
  bool addChunk();
  bool allocateIndex();
  void freeIndexAndScratch();
  void freeAuxiliary();
};
