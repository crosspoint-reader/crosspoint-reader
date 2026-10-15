#pragma once

#include <NorFlash.h>

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "GlyphStoreFormat.h"

/// Glyph records staged in a caller-owned buffer exactly as they will sit in the log, so
/// FlashGlyphStore::commit() writes a whole batch with ONE program call. On the X3 each write call costs
/// about 100 µs on top of its bytes, and one call per batch also programs each flash page once rather than
/// once per record. On boards with PSRAM, keep the buffer in internal RAM (flash writes from PSRAM go in
/// 32-byte pieces).
class GlyphBatch {
 public:
  GlyphBatch(uint8_t* buffer, const uint32_t capacity) : buffer_(buffer), capacity_(capacity) {}

  /// Stages a record for `key` with `metrics` (its dataOffset is set at commit) and returns where the caller
  /// writes the bitmap: all metrics.dataLength bytes of it, since they start as 0xFF, which draws as solid ink.
  /// Returns nullptr when `key` is not a glyph key, the bitmap's length does not match its width and height
  /// or is over glyphstore::kMaxBitmapBytes, or the buffer has no room left (commit, then add again). Keys in
  /// one batch must be distinct and not yet stored.
  uint8_t* add(uint32_t key, const EpdGlyph& metrics);
  /// Takes back the record of the last add() (its bitmap could not be drawn). Only once per add().
  void dropLast();

  uint16_t count() const { return count_; }
  void clear() {
    used_ = 0;
    count_ = 0;
    lastUsed_ = 0;
  }

 private:
  friend class FlashGlyphStore;

  uint8_t* buffer_;
  uint32_t capacity_;
  uint32_t used_ = 0;
  uint32_t lastUsed_ = 0;  // used_ before the last add()
  uint16_t count_ = 0;
};

/// The persistent TTF glyph store in the `spiffs` partition:
/// - a write-once hash index and an append-only log of rendered glyphs;
/// - a kerning pair index, and a size table that gives each pixel size a small index for the glyph keys;
/// - the font's own bytes, in style blocks.
///
/// Rules it keeps:
/// - Every byte is programmed at most once between erases.
/// - A record is written before the slot that points at it, so a power cut leaves at worst bytes that
///   nothing references, and the next open resumes past them.
/// - Erases happen only at install and wipe, and only on sectors that hold data.
/// - A lookup reads mapped flash in place: a hit is a pointer to the EpdGlyph inside the log.
///
/// Lifetimes: every pointer the store hands out (find()'s EpdGlyph, logBase(), mapStyle()) stays valid until
/// close(), open(), beginInstall() or wipe(). The NorFlash must outlive the store.
///
/// Not thread-safe: TtfFont calls it only under RenderLock.
class FlashGlyphStore {
 public:
  /// What open() found, and what the caller does about it:
  /// - Ready.
  /// - NotInstalled, OtherFamily: install the family.
  /// - MapFailed: the mapping budget shrank since the install (a firmware update); install again with a
  ///   fresh plan, or render without the store for this session.
  /// - NoMemory: render without the store for now; never a reason to install again.
  enum class OpenResult : uint8_t { Ready, NotInstalled, OtherFamily, MapFailed, NoMemory };
  /// Found: a stored glyph. NoGlyph: the font has no glyph for this key (markNoGlyph()). NotStored: render it.
  enum class Lookup : uint8_t { NotStored, NoGlyph, Found };
  /// What commit() and markNoGlyph() did:
  /// - Stored: everything is findable.
  /// - Refused: nothing was written. A key was not a glyph key or used a size index not claimed since the
  ///   last wipe, or the store is not open. Wipe pending does not change.
  /// - Full, WriteFailed: the store is full, or a write failed. Part of a batch may have been stored
  ///   (find() tells). Wipe is now pending, and later writes are refused with Full until wipe().
  enum class Commit : uint8_t { Stored, Refused, Full, WriteFailed };

  explicit FlashGlyphStore(NorFlash& flash) : flash_(flash) {}
  ~FlashGlyphStore() { close(); }
  FlashGlyphStore(const FlashGlyphStore&) = delete;
  FlashGlyphStore& operator=(const FlashGlyphStore&) = delete;

  // --- Install ---

  /// Erases the header first, so a power cut from here on reads as "not installed". Then erases every
  /// sector of the new layout that holds data. SD mode needs `sdInfo`: each style's file hash, size and table
  /// map (table entries must lie inside their style block); offsets and sizes come from `layout`.
  bool beginInstall(const glyphstore::Layout& layout, const uint8_t identity[glyphstore::kIdentityBytes],
                    const glyphstore::StyleInfo* sdInfo);
  /// Copies style data into a style block, each byte once. Only between beginInstall() and finishInstall().
  bool writeStyleData(uint8_t style, uint32_t offset, const void* data, uint32_t len);
  /// Writes the header. open() succeeds from now on.
  bool finishInstall();

  // --- Session ---

  /// Maps the store and finds where writing resumes.
  OpenResult open(const uint8_t identity[glyphstore::kIdentityBytes]);
  void close();
  bool isOpen() const { return open_; }
  uint8_t mode() const { return geometry_.mode; }
  uint8_t styleCount() const { return geometry_.styleCount; }
  /// EpdFontData::bitmap for every TTF face: EpdGlyph::dataOffset is relative to it.
  const uint8_t* logBase() const { return log_; }
  /// A style's data, mapped. Flash mode maps one style at a time: the pointer stays valid until mapStyle()
  /// is called for another style, so re-fetch it after a miss in another style. SD mode maps all styles
  /// together.
  const uint8_t* mapStyle(uint8_t style);
  /// A style's header record (SD mode: its table map). False when not open.
  bool styleInfo(uint8_t style, glyphstore::StyleInfo& out) const;
  /// Bumped by every wipe(). A face that holds a size index re-claims it with sizeIndex() when this changed.
  uint32_t generation() const { return generation_; }

  // --- Glyphs ---

  Lookup find(uint32_t key, const EpdGlyph** glyph) const;
  /// Writes the staged records with one program call, then one slot per record, and clears the batch.
  Commit commit(GlyphBatch& batch);
  /// Records that the font has no glyph for this key: a slot with no record.
  Commit markNoGlyph(uint32_t key);

  // --- Sizes ---

  /// The size-table index for a pixels-per-em (26.6), claiming a free entry (a flash write) on first use.
  /// False when all entries are taken, on a write error, or for a new size while wipe is pending. Taking the last
  /// entry makes the wipe pending but stops nothing else: the claimed sizes go on storing their glyphs until the wipe.
  /// A wipe empties the table: indices claimed before it are void, and commits under them are refused until
  /// they are claimed again.
  bool sizeIndex(uint32_t ppem26_6, uint8_t* index);

  // --- Kerning pairs ---

  bool findPair(uint16_t left, uint16_t right, uint8_t style, int16_t* fontUnits) const;
  /// Stores a pair's kerning in font units. False when it was not stored: the pair index is full (pairs then
  /// come from the GPOS reader every time; never a reason to wipe), writes are stopped, or a write failed (which
  /// stops them).
  bool storePair(uint16_t left, uint16_t right, uint8_t style, int16_t fontUnits);

  // --- A full store, and the wipe ---

  bool wipePending() const { return wipePending_; }
  /// Nothing is written until the next wipe: the log or the index filled, a write failed, or the store opened
  /// with a wipe pending. A full size table alone does not stop writes.
  bool writesStopped() const { return writesStopped_; }
  /// Records wipe pending in flash, then erases the index, the pair index and the log (only sectors that hold
  /// data) and last the state sector (size table and wipe word). A cut anywhere leaves wipe pending, and the
  /// next wipe() finishes the job. Call it only where no pointer into the log and no size index is in use:
  /// at book open, or before a section build. False: not done; it stays pending.
  bool wipe();

  // --- Stats ---

  uint32_t logUsed() const { return logUsed_; }
  uint32_t usedSlots() const { return usedSlots_; }
  uint32_t usedPairSlots() const { return usedPairs_; }
  const glyphstore::Geometry& geometry() const { return geometry_; }

 private:
  enum class Insert : uint8_t { Done, ChainFull, WriteFailed };

  // The EpdGlyph of the record at `offset` in the log, read in place (4-byte aligned in mapped flash).
  const EpdGlyph& recordGlyph(const uint32_t offset) const {
    return *reinterpret_cast<const EpdGlyph*>(__builtin_assume_aligned(log_ + offset + 4, 4));
  }
  bool plausibleRecord(uint32_t offset) const;
  const EpdGlyph* validRecord(uint32_t key, uint32_t dataOffset) const;
  uint32_t scanLog() const;
  Insert insert(uint32_t tableOffset, const uint8_t* table, uint32_t slots, uint32_t key, uint32_t value);
  Commit insertGlyphSlot(uint32_t key, uint32_t value);
  Commit writeBatch(GlyphBatch& batch);
  bool sizeClaimed(uint32_t key) const { return (claimedSizes_ >> glyphstore::keySizeIndex(key)) & 1u; }
  bool readSizeEntry(uint8_t i, glyphstore::SizeEntry& entry) const;
  bool eraseWritten(uint32_t offset, uint32_t len);
  bool isBlank(uint32_t sector) const;
  void stopWrites();
  bool recordWipePending();

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
  uint32_t claimedSizes_ = 0;  // bit i: size entry i is claimed
  uint32_t generation_ = 0;
  bool open_ = false;
  bool wipePending_ = false;
  bool writesStopped_ = false;
  std::unique_ptr<glyphstore::Header> installing_;  // written by finishInstall()
};
