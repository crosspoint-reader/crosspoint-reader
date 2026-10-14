#pragma once

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "GlyphStoreFormat.h"
#include "NorFlash.h"

/// Glyph records staged in a caller-owned buffer exactly as they will sit in the log, so
/// FlashGlyphStore::commit() writes a whole batch with ONE program call. On the X3 each write call costs
/// about 100 µs on top of its bytes (spec Appendix A), and one call per batch also programs each flash
/// page once rather than once per record.
class GlyphBatch {
 public:
  GlyphBatch(uint8_t* buffer, const uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}

  /// Stages a record for `key` with `metrics` (its dataOffset is set at commit). Returns where the caller
  /// writes the metrics.dataLength bitmap bytes, or nullptr when the buffer has no room (commit, then add
  /// again) or the bitmap is over glyphstore::kMaxBitmapBytes. Keys in one batch must be distinct and not
  /// yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);

  uint16_t count() const { return count_; }
  uint32_t bytes() const { return used_; }
  void clear() {
    used_ = 0;
    count_ = 0;
  }

 private:
  friend class FlashGlyphStore;

  uint8_t* buffer_;
  uint32_t capacity_;
  uint32_t used_ = 0;
  uint16_t count_ = 0;
};

/// The persistent TTF glyph store in the `spiffs` partition (spec section 4):
/// - a write-once hash index and an append-only log of rendered glyphs;
/// - a kerning pair index and a size table;
/// - the font's own bytes, in style blocks.
///
/// Rules it keeps:
/// - Outside the header sector, every byte is programmed at most once between erases.
/// - A record is written before the slot that points at it, so a power cut leaves at worst bytes that
///   nothing references.
/// - Erases happen only at install and wipe, and only on sectors that hold data.
/// - A lookup reads mapped flash in place: a hit is a pointer to the EpdGlyph inside the log.
///
/// Not thread-safe: TtfFont calls it only under RenderLock (spec 3.4).
class FlashGlyphStore {
 public:
  enum class OpenResult : uint8_t { Ready, NotInstalled, OtherFamily, MapFailed, NoMemory };
  enum class Lookup : uint8_t { Absent, Missing, Found };
  enum class Commit : uint8_t { Stored, Full, WriteFailed };

  explicit FlashGlyphStore(NorFlash& flash) : flash_(flash) {}
  ~FlashGlyphStore() { close(); }
  FlashGlyphStore(const FlashGlyphStore&) = delete;
  FlashGlyphStore& operator=(const FlashGlyphStore&) = delete;

  // --- Install (4.1, 7.1) ---

  /// Erases the header first, so a power cut from here on reads as "not installed". Then erases every
  /// sector of the new layout that holds data. `sdInfo` (SD mode, may be null) supplies each style's file
  /// hash, size and table map; offsets and sizes come from `layout`.
  bool beginInstall(const glyphstore::Layout& layout, const uint8_t identity[16], const glyphstore::StyleInfo* sdInfo);
  /// Copies style data into a style block, each byte once. Only between beginInstall() and finishInstall().
  bool writeStyleData(uint8_t style, uint32_t offset, const void* data, uint32_t len);
  /// Writes the header. Only now does the store open.
  bool finishInstall();

  // --- Session ---

  OpenResult open(const uint8_t identity[16]);
  void close();
  bool isOpen() const { return open_; }
  uint8_t mode() const { return geometry_.mode; }
  uint8_t styleCount() const { return geometry_.styleCount; }
  /// EpdFontData::bitmap for every TTF face: EpdGlyph::dataOffset is relative to it.
  const uint8_t* logBase() const { return log_; }
  /// A style's data, mapped. Flash mode maps one style at a time: the pointer stays valid until
  /// mapStyle() is called for another style, or the store closes. SD mode maps all styles together.
  const uint8_t* mapStyle(uint8_t style);
  /// A style's header record (SD mode: its table map). False when not open.
  bool styleInfo(uint8_t style, glyphstore::StyleInfo& out) const;

  // --- Glyphs (4.3-4.5) ---

  Lookup find(uint32_t key, const EpdGlyph** glyph) const;
  /// Writes the staged records with one program call, then one slot per record, and clears the batch.
  /// - Full: nothing new was stored, and wipe is now pending (4.6).
  /// - WriteFailed: a write failed, and wipe is now pending (6.2).
  Commit commit(GlyphBatch& batch);
  /// Records that the font has no glyph for this key (6.1): a slot with no record.
  Commit markMissing(uint32_t key);

  // --- Sizes (4.2) ---

  /// The size-table index for a pixels-per-em (26.6), claiming a free entry on first use. -1 when the
  /// table is full (wipe is then pending), on a write error, or for a new size while wipe is pending.
  int sizeIndex(uint32_t ppem26_6, uint8_t flags);

  // --- Kerning pairs (4.3) ---

  bool findPair(uint16_t left, uint16_t right, uint8_t style, int16_t* fontUnits) const;
  /// Stores a pair's kerning in font units. Returns false when it was not stored: the pair index is full
  /// (pairs then come from the GPOS reader every time; never a reason to wipe), wipe is pending, or a
  /// write failed.
  bool storePair(uint16_t left, uint16_t right, uint8_t style, int16_t fontUnits);

  // --- A full store, and the wipe (4.6, 7.5) ---

  bool wipePending() const { return wipePending_; }
  /// Erases the index, the pair index and the log (only sectors that hold data), then rewrites the header
  /// with an empty size table and no wipe pending. Call it only where no EpdGlyph pointer into the log can
  /// be live: at book open, or before a section build.
  bool wipe();

  // --- Stats ---

  uint32_t logUsed() const { return logUsed_; }
  uint32_t usedSlots() const { return usedSlots_; }
  uint32_t usedPairSlots() const { return usedPairs_; }
  const glyphstore::Geometry& geometry() const { return geometry_; }

 private:
  enum class Insert : uint8_t { Done, ChainFull, WriteFailed };

  bool plausibleRecord(uint32_t offset) const;
  const EpdGlyph* validRecord(uint32_t key, uint32_t logOffset) const;
  uint32_t scanLog() const;
  Insert insert(uint32_t tableOffset, const uint8_t* table, uint32_t slots, uint32_t key, uint32_t value);
  bool eraseWritten(uint32_t offset, uint32_t len);
  bool isBlank(uint32_t sector);
  void setWipePending();

  NorFlash& flash_;
  glyphstore::Geometry geometry_{};
  uint32_t styleOffset_[glyphstore::kMaxStyles] = {};
  uint32_t styleSize_[glyphstore::kMaxStyles] = {};
  const uint8_t* region_ = nullptr;  // one mapping: index, pair index, log
  const uint8_t* index_ = nullptr;
  const uint8_t* pairs_ = nullptr;
  const uint8_t* log_ = nullptr;
  const uint8_t* styleMap_ = nullptr;
  uint8_t mappedStyle_ = 0xFF;
  uint32_t logUsed_ = 0;
  uint32_t usedSlots_ = 0;
  uint32_t usedPairs_ = 0;
  bool open_ = false;
  bool wipePending_ = false;
  std::unique_ptr<glyphstore::Header> installing_;  // written by finishInstall()
};
