#include "FlashGlyphStore.h"

#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstring>

using namespace glyphstore;

static_assert(sizeof(EpdGlyph) + 4 == kRecordHeaderBytes, "a record header is the key plus one EpdGlyph");
static_assert(offsetof(EpdGlyph, dataOffset) % 4 == 0, "EpdGlyph::dataOffset must be readable in place");
static_assert(NorFlash::kSectorBytes == kSectorBytes && NorFlash::kMapPageBytes == kPageBytes,
              "the format and the flash agree on sector and page sizes");

namespace {

// Loads from mapped flash. Each is 4-byte aligned: mappings start on a page, and every offset the store
// reads at is a multiple of 4. Saying so lets the compiler emit plain loads; without it RISC-V calls
// memcpy for every field, which costs more than the lookup itself. A record's EpdGlyph is read in place
// (recordGlyph()) for the same reason: copying 16 bytes out is a memcpy call too.
uint32_t flashU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, __builtin_assume_aligned(bytes, 4), sizeof value);
  return value;
}

Slot flashSlot(const uint8_t* table, const uint32_t i) {
  Slot slot;
  std::memcpy(&slot, __builtin_assume_aligned(table + static_cast<size_t>(i) * kSlotBytes, 4), sizeof slot);
  return slot;
}

// Loads from a caller's batch buffer, which need not be aligned.
uint32_t ramU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof value);
  return value;
}

EpdGlyph ramGlyph(const uint8_t* bytes) {
  EpdGlyph glyph;
  std::memcpy(&glyph, bytes, sizeof glyph);
  return glyph;
}

bool slotErased(const Slot& slot) { return slot.key == kErased && slot.value == kErased; }

uint32_t nextSlot(const uint32_t i, const uint32_t slots) { return i + 1 == slots ? 0 : i + 1; }

uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }

// The most entries a table of `capacity` takes before it counts as full.
uint32_t limit(const uint32_t capacity, const uint32_t percent) {
  return static_cast<uint32_t>(uint64_t{capacity} * percent / 100);
}

uint32_t countUsed(const uint8_t* table, const uint32_t slots) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < slots; ++i) {
    if (!slotErased(flashSlot(table, i))) ++used;
  }
  return used;
}

// The regions are sector-aligned, in the order index, pairs, log, after the header and state sectors, and
// inside the flash. Each table is at least kMaxProbe slots, so a probe never wraps onto itself.
bool validGeometry(const Geometry& g, const uint32_t flashSize) {
  if (g.mode > kModeSd || g.styleCount == 0 || g.styleCount > kMaxStyles) return false;
  if (g.indexSlots < kMaxProbe || g.pairSlots < kMaxProbe || g.logSize == 0 || g.logSize % kSectorBytes != 0) {
    return false;
  }
  if (g.indexOffset % kSectorBytes != 0 || g.pairOffset % kSectorBytes != 0 || g.logOffset % kSectorBytes != 0) {
    return false;
  }
  const uint64_t indexEnd = uint64_t{g.indexOffset} + uint64_t{g.indexSlots} * kSlotBytes;
  const uint64_t pairEnd = uint64_t{g.pairOffset} + uint64_t{g.pairSlots} * kSlotBytes;
  return g.indexOffset >= kFirstRegionOffset && indexEnd <= g.pairOffset && pairEnd <= g.logOffset &&
         uint64_t{g.logOffset} + g.logSize <= flashSize;
}

// The style blocks are sector-aligned, in order, between the state sector and the index, and every table
// entry of a style lies inside its block.
bool validStyles(const StyleInfo* styles, const Geometry& g) {
  uint32_t floor = kFirstRegionOffset;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    const StyleInfo& style = styles[s];
    if (style.offset < floor || style.offset % kSectorBytes != 0 || style.size == 0) return false;
    if (uint64_t{style.offset} + style.size > g.indexOffset || style.tableCount > kMaxTables) return false;
    for (uint32_t t = 0; t < style.tableCount; ++t) {
      if (uint64_t{style.tables[t].blockOffset} + style.tables[t].length > style.size) return false;
    }
    floor = style.offset + style.size;
  }
  return true;
}

bool validHeader(const Header& h, const uint32_t flashSize) {
  return h.magic == kMagic && h.version == kFormatVersion && h.headerBytes == sizeof(Header) &&
         h.check == fnv1a(&h, offsetof(Header, check)) && validGeometry(h.geometry, flashSize) &&
         validStyles(h.styles, h.geometry);
}

}  // namespace

// --- Install ---

bool FlashGlyphStore::beginInstall(const Layout& layout, const uint8_t identity[kIdentityBytes],
                                   const StyleInfo* sdInfo) {
  close();
  const Geometry& g = layout.geometry;
  if (!validGeometry(g, flash_.size()) || (g.mode == kModeSd && !sdInfo)) {
    LOG_ERR("GST", "install: the layout does not fit the flash, or SD mode lacks its table maps");
    return false;
  }
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "install: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  header->magic = kMagic;
  header->version = kFormatVersion;
  header->headerBytes = sizeof(Header);
  std::memcpy(header->identity, identity, kIdentityBytes);
  header->geometry = g;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    if (sdInfo) header->styles[s] = sdInfo[s];
    header->styles[s].offset = layout.styleOffset[s];
    header->styles[s].size = layout.styleSize[s];
  }
  if (!validStyles(header->styles, g)) {
    LOG_ERR("GST", "install: the style blocks or their tables do not fit");
    return false;
  }

  // The header sector goes first: from here until finishInstall() the store reads as not installed.
  bool erased = eraseWritten(0, kFirstRegionOffset);
  for (uint8_t s = 0; erased && s < g.styleCount; ++s) {
    erased = eraseWritten(layout.styleOffset[s], alignUp(layout.styleSize[s], kSectorBytes));
  }
  erased = erased && eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) &&
           eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) && eraseWritten(g.logOffset, g.logSize);
  if (!erased) {
    LOG_ERR("GST", "install: erase failed");
    return false;
  }
  installing_ = std::move(header);
  return true;
}

bool FlashGlyphStore::writeStyleData(const uint8_t style, const uint32_t offset, const void* data, const uint32_t len) {
  if (!installing_ || style >= installing_->geometry.styleCount) return false;
  const StyleInfo& info = installing_->styles[style];
  if (offset > info.size || len > info.size - offset) return false;
  return flash_.program(info.offset + offset, data, len);
}

bool FlashGlyphStore::finishInstall() {
  if (!installing_) return false;
  installing_->check = fnv1a(installing_.get(), offsetof(Header, check));
  const bool written = flash_.program(0, installing_.get(), sizeof(Header));
  installing_.reset();
  if (!written) {
    LOG_ERR("GST", "install: header write failed");
  }
  return written;
}

// --- Session ---

FlashGlyphStore::OpenResult FlashGlyphStore::open(const uint8_t identity[kIdentityBytes]) {
  close();
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "open: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return OpenResult::NoMemory;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || !validHeader(*header, flash_.size())) {
    return OpenResult::NotInstalled;
  }
  if (std::memcmp(header->identity, identity, kIdentityBytes) != 0) return OpenResult::OtherFamily;
  const Geometry& g = header->geometry;
  region_ = flash_.map(g.indexOffset, g.logOffset + g.logSize - g.indexOffset);
  if (!region_) {
    LOG_ERR("GST", "open: cannot map the index and log (%u B, %u map pages free)",
            static_cast<unsigned>(g.logOffset + g.logSize - g.indexOffset),
            static_cast<unsigned>(flash_.freeMapPages()));
    return OpenResult::MapFailed;
  }
  geometry_ = g;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    styleOffset_[s] = header->styles[s].offset;
    styleSize_[s] = header->styles[s].size;
  }
  index_ = region_;
  pairs_ = region_ + (g.pairOffset - g.indexOffset);
  log_ = region_ + (g.logOffset - g.indexOffset);
  uint32_t wipeWord = kErased;
  if (!flash_.read(kWipeWordOffset, &wipeWord, sizeof wipeWord)) wipeWord = 0;  // unreadable: assume pending
  wipePending_ = wipeWord != kErased;
  writesStopped_ = wipePending_;  // what filled the store, or failed, last session is not known
  for (uint8_t i = 0; i < kSizeEntries; ++i) {
    SizeEntry entry{};
    if (readSizeEntry(i, entry) && entry.claimed == 0) claimedSizes_ |= 1u << i;
  }
  logUsed_ = scanLog();
  usedSlots_ = countUsed(index_, g.indexSlots);
  usedPairs_ = countUsed(pairs_, g.pairSlots);
  open_ = true;
  LOG_INF("GST", "open: log %u of %u B, %u of %u slots, %u pairs%s", static_cast<unsigned>(logUsed_),
          static_cast<unsigned>(g.logSize), static_cast<unsigned>(usedSlots_), static_cast<unsigned>(g.indexSlots),
          static_cast<unsigned>(usedPairs_), wipePending_ ? ", wipe pending" : "");
  return OpenResult::Ready;
}

void FlashGlyphStore::close() {
  if (styleMap_) flash_.unmap(styleMap_);
  if (region_) flash_.unmap(region_);
  styleMap_ = nullptr;
  mappedStyle_ = 0xFF;
  region_ = index_ = pairs_ = log_ = nullptr;
  geometry_ = Geometry{};
  logUsed_ = usedSlots_ = usedPairs_ = claimedSizes_ = 0;
  open_ = false;
  wipePending_ = false;
  writesStopped_ = false;
  installing_.reset();  // an install that was never finished
}

const uint8_t* FlashGlyphStore::mapStyle(const uint8_t style) {
  if (!open_ || style >= geometry_.styleCount) return nullptr;
  if (geometry_.mode == kModeSd) {
    // All styles' tables map together: the budget counted them all.
    if (!styleMap_) {
      const uint8_t last = geometry_.styleCount - 1;
      styleMap_ = flash_.map(styleOffset_[0], styleOffset_[last] + styleSize_[last] - styleOffset_[0]);
      if (!styleMap_) {
        LOG_ERR("GST", "cannot map the copied tables");
        return nullptr;
      }
    }
    return styleMap_ + (styleOffset_[style] - styleOffset_[0]);
  }
  // Flash mode: one style at a time, so the budget holds only the largest.
  if (styleMap_ && mappedStyle_ == style) return styleMap_;
  if (styleMap_) flash_.unmap(styleMap_);
  styleMap_ = flash_.map(styleOffset_[style], styleSize_[style]);
  mappedStyle_ = styleMap_ ? style : 0xFF;
  if (!styleMap_) {
    LOG_ERR("GST", "cannot map style %u (%u B)", style, static_cast<unsigned>(styleSize_[style]));
  }
  return styleMap_;
}

bool FlashGlyphStore::styleInfo(const uint8_t style, StyleInfo& out) const {
  if (!open_ || style >= geometry_.styleCount) return false;
  return flash_.read(static_cast<uint32_t>(offsetof(Header, styles) + style * sizeof(StyleInfo)), &out, sizeof out);
}

// --- The log ---

// A record the scan can step over: a glyph key, a bitmap length that matches its size and the cap, and a
// dataOffset that points just past its own header. A header torn by a power cut fails one of these.
bool FlashGlyphStore::plausibleRecord(const uint32_t offset) const {
  if (offset % 4 != 0 || offset > geometry_.logSize || geometry_.logSize - offset < kRecordHeaderBytes) return false;
  if (!isGlyphKey(flashU32(log_ + offset))) return false;
  const EpdGlyph& glyph = recordGlyph(offset);
  return glyph.dataLength <= kMaxBitmapBytes && glyph.dataLength == glyphDataBytes(glyph.width, glyph.height, true) &&
         glyph.dataOffset == offset + kRecordHeaderBytes && recordBytes(glyph.dataLength) <= geometry_.logSize - offset;
}

uint32_t FlashGlyphStore::scanLog() const {
  const uint32_t size = geometry_.logSize;
  uint32_t offset = 0;
  for (;;) {
    while (plausibleRecord(offset)) offset += recordBytes(recordGlyph(offset).dataLength);
    // The walk stopped at an erased key, a torn record or the end. A cut write spans at most one record, and
    // can have left programmed bytes after the stop, even behind a key that still reads erased.
    const uint32_t window = size - offset < kMaxRecordBytes ? size : offset + kMaxRecordBytes;
    uint32_t end = offset;  // one past the last programmed byte in the window
    for (uint32_t i = window; i > offset; --i) {
      if (log_[i - 1] != 0xFF) {
        end = i;
        break;
      }
    }
    if (end == offset) return offset;  // nothing after the stop: writing resumes here
    // Every session after a torn write resumed on a sector boundary past it. If one of the boundaries
    // up to the programmed bytes starts a record, the walk carries on there; otherwise it goes on past
    // them, at the next boundary. Without this, a later boot would stop at the same torn record and
    // write over what those sessions stored.
    uint32_t next = alignUp(end, kSectorBytes);
    for (uint32_t boundary = alignUp(offset + 1, kSectorBytes); boundary < next; boundary += kSectorBytes) {
      if (plausibleRecord(boundary)) {
        next = boundary;
        break;
      }
    }
    if (next >= size) return size;
    LOG_INF("GST", "log: torn write at %u; carrying on at %u", static_cast<unsigned>(offset),
            static_cast<unsigned>(next));
    offset = next;
  }
}

// --- Erasing ---

// Erases are what wear the chip, and one costs 4-6 ms even on a blank sector (measured on the X3), so only
// sectors that hold data are erased; neighbouring ones go in one call.
bool FlashGlyphStore::eraseWritten(const uint32_t offset, const uint32_t len) {
  uint32_t runStart = 0;
  uint32_t runLen = 0;
  for (uint32_t sector = offset; sector < offset + len; sector += kSectorBytes) {
    if (!isBlank(sector)) {
      if (runLen == 0) runStart = sector;
      runLen += kSectorBytes;
      continue;
    }
    if (runLen != 0 && !flash_.erase(runStart, runLen)) return false;
    runLen = 0;
  }
  return runLen == 0 || flash_.erase(runStart, runLen);
}

// Through the mapping when the sector is in it (a wipe), else by reading small chunks (an install).
bool FlashGlyphStore::isBlank(const uint32_t sector) const {
  if (region_ && sector >= geometry_.indexOffset && sector + kSectorBytes <= geometry_.logOffset + geometry_.logSize) {
    const uint8_t* bytes = region_ + (sector - geometry_.indexOffset);
    for (uint32_t i = 0; i < kSectorBytes; i += 4) {
      if (flashU32(bytes + i) != kErased) return false;
    }
    return true;
  }
  uint8_t chunk[256];
  for (uint32_t at = 0; at < kSectorBytes; at += sizeof chunk) {
    if (!flash_.read(sector + at, chunk, sizeof chunk)) return false;  // unreadable: erase it
    for (const uint8_t b : chunk) {
      if (b != 0xFF) return false;
    }
  }
  return true;
}

// --- Glyphs ---

uint8_t* GlyphBatch::add(const uint32_t key, const EpdGlyph& metrics) {
  // A record whose key or length is malformed would read as torn to the log scan.
  if (!isGlyphKey(key) || metrics.dataLength > kMaxBitmapBytes ||
      metrics.dataLength != glyphDataBytes(metrics.width, metrics.height, true)) {
    return nullptr;
  }
  const uint32_t bytes = recordBytes(metrics.dataLength);
  if (bytes > capacity_ - used_) return nullptr;
  uint8_t* record = buffer_ + used_;
  std::memset(record, 0xFF, bytes);  // the padding stays erased
  std::memcpy(record, &key, sizeof key);
  EpdGlyph glyph = metrics;
  glyph.dataOffset = 0;  // known at commit, when the record's place in the log is
  std::memcpy(record + 4, &glyph, sizeof glyph);
  lastUsed_ = used_;
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}

void GlyphBatch::dropLast() {
  if (count_ == 0 || used_ == lastUsed_) return;
  used_ = lastUsed_;
  --count_;
}

// The record a glyph slot points at, if it is complete and really this key's: the slot value is the record's
// dataOffset, inside the written log.
const EpdGlyph* FlashGlyphStore::validRecord(const uint32_t key, const uint32_t dataOffset) const {
  if (dataOffset < kRecordHeaderBytes || dataOffset > logUsed_ || dataOffset % 4 != 0) return nullptr;
  const uint32_t offset = dataOffset - kRecordHeaderBytes;
  if (flashU32(log_ + offset) != key || !plausibleRecord(offset)) return nullptr;
  const EpdGlyph& glyph = recordGlyph(offset);
  if (recordBytes(glyph.dataLength) > logUsed_ - offset) return nullptr;
  return &glyph;
}

FlashGlyphStore::Lookup FlashGlyphStore::find(const uint32_t key, const EpdGlyph** glyph) const {
  if (!open_ || !isGlyphKey(key)) return Lookup::NotStored;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(index_, i);
    if (slotErased(slot)) return Lookup::NotStored;
    if (slot.key != key) continue;
    if (slot.value == kNoGlyph) return Lookup::NoGlyph;
    if (const EpdGlyph* found = validRecord(key, slot.value)) {
      if (glyph) *glyph = found;
      return Lookup::Found;
    }
    // A slot cut short, or one whose record never completed: keep probing.
  }
  return Lookup::NotStored;
}

FlashGlyphStore::Insert FlashGlyphStore::insert(const uint32_t tableOffset, const uint8_t* table, const uint32_t slots,
                                                const uint32_t key, const uint32_t value) {
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    // A slot in use, or one cut short (some bits programmed), is never programmed again.
    if (!slotErased(flashSlot(table, i))) continue;
    const Slot slot{key, value};
    return flash_.program(tableOffset + i * kSlotBytes, &slot, sizeof slot) ? Insert::Done : Insert::WriteFailed;
  }
  return Insert::ChainFull;
}

// One glyph slot (a record's dataOffset, or kNoGlyph), and what a failure means for the store.
FlashGlyphStore::Commit FlashGlyphStore::insertGlyphSlot(const uint32_t key, const uint32_t value) {
  switch (insert(geometry_.indexOffset, index_, geometry_.indexSlots, key, value)) {
    case Insert::Done:
      ++usedSlots_;
      return Commit::Stored;
    case Insert::ChainFull:
      LOG_INF("GST", "full: a probe chain passed %u slots; wipe pending", static_cast<unsigned>(kMaxProbe));
      stopWrites();
      return Commit::Full;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "slot write failed; wipe pending");
  stopWrites();
  return Commit::WriteFailed;
}

FlashGlyphStore::Commit FlashGlyphStore::commit(GlyphBatch& batch) {
  const Commit result = batch.count_ == 0 ? Commit::Stored : writeBatch(batch);
  batch.clear();  // the only place a batch is cleared, so no record is ever written twice
  return result;
}

FlashGlyphStore::Commit FlashGlyphStore::writeBatch(GlyphBatch& batch) {
  if (!open_) return Commit::Refused;
  for (uint32_t at = 0; at < batch.used_; at += recordBytes(ramGlyph(batch.buffer_ + at + 4).dataLength)) {
    if (!sizeClaimed(ramU32(batch.buffer_ + at))) {
      LOG_ERR("GST", "commit refused: a key uses a size index not claimed since the last wipe");
      return Commit::Refused;
    }
  }
  if (writesStopped_) return Commit::Full;
  if (logUsed_ + batch.used_ > limit(geometry_.logSize, kLogFullPercent) ||
      usedSlots_ + batch.count_ > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    LOG_INF("GST", "full: log %u of %u B, %u of %u slots; wipe pending", static_cast<unsigned>(logUsed_),
            static_cast<unsigned>(geometry_.logSize), static_cast<unsigned>(usedSlots_),
            static_cast<unsigned>(geometry_.indexSlots));
    stopWrites();
    return Commit::Full;
  }
  // Each bitmap's offset is relative to the log base, known now.
  const uint32_t base = logUsed_;
  for (uint32_t at = 0; at < batch.used_;) {
    EpdGlyph glyph = ramGlyph(batch.buffer_ + at + 4);
    glyph.dataOffset = base + at + kRecordHeaderBytes;
    std::memcpy(batch.buffer_ + at + 4, &glyph, sizeof glyph);
    at += recordBytes(glyph.dataLength);
  }
  // Every record in one program call...
  if (!flash_.program(geometry_.logOffset + base, batch.buffer_, batch.used_)) {
    LOG_ERR("GST", "log write of %u B at %u failed; wipe pending", static_cast<unsigned>(batch.used_),
            static_cast<unsigned>(base));
    stopWrites();
    return Commit::WriteFailed;
  }
  logUsed_ = base + batch.used_;
  // ...then the slots, so a probe never finds a slot whose record is incomplete. A slot holds its record's
  // dataOffset, never 0: a cut can only leave extra 1 bits, so a torn slot never reads as kNoGlyph.
  for (uint32_t at = 0; at < batch.used_;) {
    const Commit slot = insertGlyphSlot(ramU32(batch.buffer_ + at), base + at + kRecordHeaderBytes);
    if (slot != Commit::Stored) return slot;
    at += recordBytes(ramGlyph(batch.buffer_ + at + 4).dataLength);
  }
  return Commit::Stored;
}

FlashGlyphStore::Commit FlashGlyphStore::markNoGlyph(const uint32_t key) {
  if (!open_ || !isGlyphKey(key) || !sizeClaimed(key)) return Commit::Refused;
  if (writesStopped_) return Commit::Full;
  if (usedSlots_ + 1 > limit(geometry_.indexSlots, kIndexLoadPercent)) {
    stopWrites();
    return Commit::Full;
  }
  return insertGlyphSlot(key, kNoGlyph);
}

// --- Sizes ---

bool FlashGlyphStore::readSizeEntry(const uint8_t i, SizeEntry& entry) const {
  return flash_.read(kSizeTableOffset + i * static_cast<uint32_t>(sizeof(SizeEntry)), &entry, sizeof entry);
}

bool FlashGlyphStore::sizeIndex(const uint32_t ppem26_6, uint8_t* index) {
  if (!open_ || ppem26_6 == kErased || index == nullptr) return false;
  int free = -1;
  uint8_t freeCount = 0;
  for (uint8_t i = 0; i < kSizeEntries; ++i) {
    SizeEntry entry{};
    if (!readSizeEntry(i, entry)) return false;
    if (entry.ppem26_6 == ppem26_6 && entry.claimed == 0) {
      *index = i;
      return true;
    }
    // Free only if wholly erased: an entry cut short is never programmed again.
    if (entry.ppem26_6 == kErased && entry.claimed == kErased) {
      if (free < 0) free = i;
      ++freeCount;
    }
  }
  if (wipePending_) return false;
  if (free < 0) {
    // Only a new size is refused: the claimed ones go on storing, and the wipe at the next book open frees the
    // table.
    LOG_INF("GST", "full: all %u sizes in use; wipe pending", static_cast<unsigned>(kSizeEntries));
    wipePending_ = true;
    recordWipePending();
    return false;
  }
  const SizeEntry claim{ppem26_6, 0};  // `claimed` lands last and marks the entry in use
  if (!flash_.program(kSizeTableOffset + static_cast<uint32_t>(free) * sizeof(SizeEntry), &claim, sizeof claim)) {
    LOG_ERR("GST", "size table write failed; wipe pending");
    stopWrites();
    return false;
  }
  claimedSizes_ |= 1u << free;
  *index = static_cast<uint8_t>(free);
  if (freeCount == 1) {
    // That was the last entry: the next book open empties the table, before a new body size needs one.
    LOG_INF("GST", "the last of %u sizes taken; wipe pending", static_cast<unsigned>(kSizeEntries));
    wipePending_ = true;
    recordWipePending();
  }
  return true;
}

// --- Kerning pairs ---

bool FlashGlyphStore::findPair(const uint16_t left, const uint16_t right, const uint8_t style,
                               int16_t* fontUnits) const {
  const uint32_t key = pairKey(left, right);
  if (!open_ || key == kErased) return false;
  const uint32_t slots = geometry_.pairSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = nextSlot(i, slots)) {
    const Slot slot = flashSlot(pairs_, i);
    if (slotErased(slot)) return false;
    // The value's top bits are written as zero, so a value cut short reads as some other style.
    if (slot.key != key || (slot.value >> 16) != style) continue;
    if (fontUnits) *fontUnits = static_cast<int16_t>(slot.value & 0xFFFF);
    return true;
  }
  return false;
}

bool FlashGlyphStore::storePair(const uint16_t left, const uint16_t right, const uint8_t style,
                                const int16_t fontUnits) {
  const uint32_t key = pairKey(left, right);
  if (!open_ || writesStopped_ || key == kErased || style > 3) return false;
  // A full pair index only stops storing pairs; they come from the GPOS reader instead (never a wipe).
  if (usedPairs_ + 1 > limit(geometry_.pairSlots, kIndexLoadPercent)) return false;
  const uint32_t value = (static_cast<uint32_t>(style) << 16) | static_cast<uint16_t>(fontUnits);
  switch (insert(geometry_.pairOffset, pairs_, geometry_.pairSlots, key, value)) {
    case Insert::Done:
      ++usedPairs_;
      return true;
    case Insert::ChainFull:
      return false;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "pair write failed; wipe pending");
  stopWrites();
  return false;
}

// --- Wipe pending, and the wipe ---

void FlashGlyphStore::stopWrites() {
  writesStopped_ = true;
  if (wipePending_) return;
  wipePending_ = true;  // holds for the session even if the flash write below fails
  recordWipePending();
}

// Programs the wipe word, unless it already reads programmed, and reads it back.
bool FlashGlyphStore::recordWipePending() {
  uint32_t word = kErased;
  if (flash_.read(kWipeWordOffset, &word, sizeof word) && word != kErased) return true;
  const uint32_t zero = 0;
  if (!flash_.program(kWipeWordOffset, &zero, sizeof zero) || !flash_.read(kWipeWordOffset, &word, sizeof word) ||
      word == kErased) {
    LOG_ERR("GST", "could not record wipe pending in flash");
    return false;
  }
  return true;
}

bool FlashGlyphStore::wipe() {
  if (!open_) return false;
  // Wipe pending reaches flash before anything is erased. Then a cut anywhere below leaves it set and the next
  // wipe() finishes the job; without it a half-erased log would read as an empty store and be written over.
  wipePending_ = true;
  writesStopped_ = true;
  if (!recordWipePending()) return false;
  const Geometry& g = geometry_;
  // Everything a lookup can reach goes first; the state sector, which holds the wipe word, goes last.
  if (!eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) || !eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) ||
      !eraseWritten(g.logOffset, g.logSize) || !flash_.erase(kStateSectorOffset, kSectorBytes)) {
    LOG_ERR("GST", "wipe: erase failed; wipe stays pending");
    return false;
  }
  logUsed_ = usedSlots_ = usedPairs_ = claimedSizes_ = 0;
  ++generation_;
  wipePending_ = false;
  writesStopped_ = false;
  return true;
}
