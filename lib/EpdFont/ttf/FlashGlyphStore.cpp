#include "FlashGlyphStore.h"

#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstring>

using namespace glyphstore;

static_assert(sizeof(EpdGlyph) + 4 == kRecordHeaderBytes, "a record header is the key plus one EpdGlyph");
static_assert(offsetof(EpdGlyph, dataOffset) % 4 == 0, "EpdGlyph::dataOffset must be readable in place");

namespace {

uint32_t loadU32(const uint8_t* bytes) {
  uint32_t value;
  std::memcpy(&value, bytes, sizeof value);
  return value;
}

EpdGlyph loadGlyph(const uint8_t* bytes) {
  EpdGlyph glyph;
  std::memcpy(&glyph, bytes, sizeof glyph);
  return glyph;
}

Slot loadSlot(const uint8_t* table, const uint32_t i) {
  Slot slot;
  std::memcpy(&slot, table + static_cast<size_t>(i) * kSlotBytes, sizeof slot);
  return slot;
}

bool slotErased(const Slot& slot) { return slot.key == kErased && slot.value == kErased; }

uint32_t recordBytes(const uint16_t dataLength) { return align4(kRecordHeaderBytes + dataLength); }

uint32_t tableBytes(const uint32_t slots) { return alignUp(slots * kSlotBytes, kSectorBytes); }

uint32_t countUsed(const uint8_t* table, const uint32_t slots) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < slots; ++i) {
    if (!slotErased(loadSlot(table, i))) ++used;
  }
  return used;
}

// The regions are sector-aligned, in the order index, pairs, log, and inside the flash.
bool validGeometry(const Geometry& g, const uint32_t flashSize) {
  if (g.mode > kModeSd || g.styleCount == 0 || g.styleCount > kMaxStyles) return false;
  if (g.indexSlots == 0 || g.pairSlots == 0 || g.logSize == 0 || g.logSize % kSectorBytes != 0) return false;
  if (g.indexOffset % kSectorBytes != 0 || g.pairOffset % kSectorBytes != 0 || g.logOffset % kSectorBytes != 0) {
    return false;
  }
  const uint64_t indexEnd = uint64_t{g.indexOffset} + uint64_t{g.indexSlots} * kSlotBytes;
  const uint64_t pairEnd = uint64_t{g.pairOffset} + uint64_t{g.pairSlots} * kSlotBytes;
  return g.indexOffset >= kSectorBytes && indexEnd <= g.pairOffset && pairEnd <= g.logOffset &&
         uint64_t{g.logOffset} + g.logSize <= flashSize;
}

// The style blocks are sector-aligned, in order, between the header sector and the index.
bool validStyles(const uint32_t* offsets, const uint32_t* sizes, const Geometry& g) {
  uint32_t floor = kSectorBytes;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    if (offsets[s] < floor || offsets[s] % kSectorBytes != 0 || sizes[s] == 0) return false;
    if (uint64_t{offsets[s]} + sizes[s] > g.indexOffset) return false;
    floor = offsets[s] + sizes[s];
  }
  return true;
}

}  // namespace

// --- Install ---

bool FlashGlyphStore::beginInstall(const Layout& layout, const uint8_t identity[16], const StyleInfo* sdInfo) {
  close();
  installing_.reset();
  const Geometry& g = layout.geometry;
  if (!validGeometry(g, flash_.size()) || !validStyles(layout.styleOffset, layout.styleSize, g)) {
    LOG_ERR("GST", "install: the layout does not fit the flash");
    return false;
  }
  for (uint8_t s = 0; sdInfo && s < g.styleCount; ++s) {
    if (sdInfo[s].tableCount > kMaxTables) {
      LOG_ERR("GST", "install: style %u lists %u tables (max %u)", s, static_cast<unsigned>(sdInfo[s].tableCount),
              static_cast<unsigned>(kMaxTables));
      return false;
    }
  }
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "install: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  header->magic = kMagic;
  header->version = kFormatVersion;
  header->headerBytes = sizeof(Header);
  std::memcpy(header->identity, identity, sizeof header->identity);
  header->geometry = g;
  for (uint8_t s = 0; s < g.styleCount; ++s) {
    if (sdInfo) header->styles[s] = sdInfo[s];
    header->styles[s].offset = layout.styleOffset[s];
    header->styles[s].size = layout.styleSize[s];
  }

  // The header sector goes first: from here until finishInstall() the store reads as not installed.
  bool erased = eraseWritten(0, kSectorBytes);
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

bool FlashGlyphStore::writeStyleData(const uint8_t style, const uint32_t offset, const void* data,
                                     const uint32_t len) {
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

FlashGlyphStore::OpenResult FlashGlyphStore::open(const uint8_t identity[16]) {
  close();
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "open: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return OpenResult::NoMemory;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || header->magic != kMagic ||
      header->version != kFormatVersion || header->headerBytes != sizeof(Header) ||
      header->check != fnv1a(header.get(), offsetof(Header, check))) {
    return OpenResult::NotInstalled;
  }
  const Geometry& g = header->geometry;
  uint32_t offsets[kMaxStyles] = {};
  uint32_t sizes[kMaxStyles] = {};
  for (uint8_t s = 0; s < kMaxStyles; ++s) {
    offsets[s] = header->styles[s].offset;
    sizes[s] = header->styles[s].size;
  }
  if (!validGeometry(g, flash_.size()) || !validStyles(offsets, sizes, g)) return OpenResult::NotInstalled;
  if (std::memcmp(header->identity, identity, sizeof header->identity) != 0) return OpenResult::OtherFamily;

  region_ = flash_.map(g.indexOffset, g.logOffset + g.logSize - g.indexOffset);
  if (!region_) {
    LOG_ERR("GST", "open: cannot map the index and log (%u B, %u map pages free)",
            static_cast<unsigned>(g.logOffset + g.logSize - g.indexOffset), static_cast<unsigned>(flash_.freeMapPages()));
    return OpenResult::MapFailed;
  }
  geometry_ = g;
  std::memcpy(styleOffset_, offsets, sizeof styleOffset_);
  std::memcpy(styleSize_, sizes, sizeof styleSize_);
  index_ = region_;
  pairs_ = region_ + (g.pairOffset - g.indexOffset);
  log_ = region_ + (g.logOffset - g.indexOffset);
  uint32_t wipeWord = kErased;
  if (!flash_.read(kWipeWordOffset, &wipeWord, sizeof wipeWord)) wipeWord = 0;  // unreadable: assume pending
  wipePending_ = wipeWord != kErased;
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
  logUsed_ = usedSlots_ = usedPairs_ = 0;
  open_ = false;
  wipePending_ = false;
}

const uint8_t* FlashGlyphStore::mapStyle(const uint8_t style) {
  if (!open_ || style >= geometry_.styleCount) return nullptr;
  if (geometry_.mode == kModeSd) {
    // All styles' tables map together: the budget counted them all (4.1).
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
  if (offset % 4 != 0 || geometry_.logSize - offset < kRecordHeaderBytes) return false;
  if (!isGlyphKey(loadU32(log_ + offset))) return false;
  const EpdGlyph glyph = loadGlyph(log_ + offset + 4);
  return glyph.dataLength <= kMaxBitmapBytes && glyph.dataLength == glyphDataBytes(glyph.width, glyph.height, true) &&
         glyph.dataOffset == offset + kRecordHeaderBytes && recordBytes(glyph.dataLength) <= geometry_.logSize - offset;
}

uint32_t FlashGlyphStore::scanLog() const {
  const uint32_t size = geometry_.logSize;
  uint32_t offset = 0;
  while (size - offset >= kRecordHeaderBytes && loadU32(log_ + offset) != kErased && plausibleRecord(offset)) {
    offset += recordBytes(loadGlyph(log_ + offset + 4).dataLength);
  }
  // Whatever stopped the scan, a cut write can have left programmed bytes after it, even behind a key that
  // still reads erased. A torn write spans at most one record, so look that far; resume past the last
  // programmed byte, at the next sector boundary, so no byte is ever programmed twice.
  const uint32_t end = size - offset < kMaxRecordBytes ? size : offset + kMaxRecordBytes;
  for (uint32_t i = end; i > offset; --i) {
    if (log_[i - 1] != 0xFF) {
      const uint32_t resume = alignUp(i, kSectorBytes);
      LOG_INF("GST", "log: torn write after %u; resuming at %u", static_cast<unsigned>(offset),
              static_cast<unsigned>(resume < size ? resume : size));
      return resume < size ? resume : size;
    }
  }
  return offset;
}

// --- Erasing ---

// Erases are what wear the chip, and one costs 4-6 ms even on a blank sector (spec Appendix A), so only
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
bool FlashGlyphStore::isBlank(const uint32_t sector) {
  if (region_ && sector >= geometry_.indexOffset && sector + kSectorBytes <= geometry_.logOffset + geometry_.logSize) {
    const uint8_t* bytes = region_ + (sector - geometry_.indexOffset);
    for (uint32_t i = 0; i < kSectorBytes; i += 4) {
      if (loadU32(bytes + i) != kErased) return false;
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
  // A record whose length does not match its size would read as torn to the log scan.
  if (metrics.dataLength > kMaxBitmapBytes ||
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
  used_ += bytes;
  ++count_;
  return record + kRecordHeaderBytes;
}

// A record a slot may point at: inside the written log, with the slot's key, and well formed.
const EpdGlyph* FlashGlyphStore::validRecord(const uint32_t key, const uint32_t logOffset) const {
  if (logOffset >= logUsed_ || logUsed_ - logOffset < kRecordHeaderBytes) return nullptr;
  if (loadU32(log_ + logOffset) != key || !plausibleRecord(logOffset)) return nullptr;
  const auto* glyph = reinterpret_cast<const EpdGlyph*>(log_ + logOffset + 4);
  if (recordBytes(glyph->dataLength) > logUsed_ - logOffset) return nullptr;
  return glyph;
}

FlashGlyphStore::Lookup FlashGlyphStore::find(const uint32_t key, const EpdGlyph** glyph) const {
  if (!open_ || !isGlyphKey(key)) return Lookup::Absent;
  const uint32_t slots = geometry_.indexSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    const Slot slot = loadSlot(index_, i);
    if (slotErased(slot)) return Lookup::Absent;
    if (slot.key != key) continue;
    if (slot.value == kMissing) return Lookup::Missing;
    if (const EpdGlyph* found = validRecord(key, slot.value)) {
      if (glyph) *glyph = found;
      return Lookup::Found;
    }
    // A slot cut short, or one whose record never completed: keep probing.
  }
  return Lookup::Absent;
}

FlashGlyphStore::Insert FlashGlyphStore::insert(const uint32_t tableOffset, const uint8_t* table,
                                                const uint32_t slots, const uint32_t key, const uint32_t value) {
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe && probe < slots; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    // A slot in use, or one cut short (some bits programmed), is never programmed again.
    if (!slotErased(loadSlot(table, i))) continue;
    const Slot slot{key, value};
    return flash_.program(tableOffset + i * kSlotBytes, &slot, sizeof slot) ? Insert::Done : Insert::WriteFailed;
  }
  return Insert::ChainFull;
}

FlashGlyphStore::Commit FlashGlyphStore::commit(GlyphBatch& batch) {
  if (batch.count_ == 0) return Commit::Stored;
  if (!open_) {
    batch.clear();
    return Commit::WriteFailed;
  }
  if (wipePending_) {
    batch.clear();
    return Commit::Full;
  }
  // 4.6: full when the batch would take the log past 95 % or the index past 70 %.
  if ((uint64_t{logUsed_} + batch.used_) * 100 > uint64_t{geometry_.logSize} * kLogFullPercent ||
      (uint64_t{usedSlots_} + batch.count_) * 100 > uint64_t{geometry_.indexSlots} * kIndexLoadPercent) {
    LOG_INF("GST", "full: log %u of %u B, %u of %u slots; wipe pending", static_cast<unsigned>(logUsed_),
            static_cast<unsigned>(geometry_.logSize), static_cast<unsigned>(usedSlots_),
            static_cast<unsigned>(geometry_.indexSlots));
    setWipePending();
    batch.clear();
    return Commit::Full;
  }
  // Each bitmap's offset is relative to the log base, known now.
  const uint32_t base = logUsed_;
  for (uint32_t at = 0; at < batch.used_;) {
    EpdGlyph glyph = loadGlyph(batch.buffer_ + at + 4);
    glyph.dataOffset = base + at + kRecordHeaderBytes;
    std::memcpy(batch.buffer_ + at + 4, &glyph, sizeof glyph);
    at += recordBytes(glyph.dataLength);
  }
  // Every record in one program call...
  if (!flash_.program(geometry_.logOffset + base, batch.buffer_, batch.used_)) {
    LOG_ERR("GST", "log write of %u B at %u failed; wipe pending", static_cast<unsigned>(batch.used_),
            static_cast<unsigned>(base));
    setWipePending();
    batch.clear();
    return Commit::WriteFailed;
  }
  logUsed_ = base + batch.used_;
  // ...then the slots, so a probe never finds a slot whose record is incomplete.
  Commit result = Commit::Stored;
  for (uint32_t at = 0; at < batch.used_;) {
    const Insert inserted =
        insert(geometry_.indexOffset, index_, geometry_.indexSlots, loadU32(batch.buffer_ + at), base + at);
    if (inserted != Insert::Done) {
      if (inserted == Insert::ChainFull) {
        LOG_INF("GST", "full: a probe chain passed %u slots; wipe pending", static_cast<unsigned>(kMaxProbe));
      } else {
        LOG_ERR("GST", "slot write failed; wipe pending");
      }
      result = inserted == Insert::ChainFull ? Commit::Full : Commit::WriteFailed;
      setWipePending();
      break;
    }
    ++usedSlots_;
    at += recordBytes(loadGlyph(batch.buffer_ + at + 4).dataLength);
  }
  batch.clear();
  return result;
}

FlashGlyphStore::Commit FlashGlyphStore::markMissing(const uint32_t key) {
  if (!open_ || !isGlyphKey(key)) return Commit::WriteFailed;
  if (wipePending_) return Commit::Full;
  if ((uint64_t{usedSlots_} + 1) * 100 > uint64_t{geometry_.indexSlots} * kIndexLoadPercent) {
    setWipePending();
    return Commit::Full;
  }
  switch (insert(geometry_.indexOffset, index_, geometry_.indexSlots, key, kMissing)) {
    case Insert::Done:
      ++usedSlots_;
      return Commit::Stored;
    case Insert::ChainFull:
      setWipePending();
      return Commit::Full;
    case Insert::WriteFailed:
      break;
  }
  LOG_ERR("GST", "slot write failed; wipe pending");
  setWipePending();
  return Commit::WriteFailed;
}

// The wipe word goes from erased to zero once; if even that write fails, wipe stays pending for the session.
void FlashGlyphStore::setWipePending() {
  if (wipePending_) return;
  wipePending_ = true;
  const uint32_t zero = 0;
  if (!flash_.program(kWipeWordOffset, &zero, sizeof zero)) {
    LOG_ERR("GST", "could not record wipe pending in flash");
  }
}

// --- Sizes ---

int FlashGlyphStore::sizeIndex(const uint32_t ppem26_6, const uint8_t flags) {
  if (!open_ || ppem26_6 == kErased) return -1;
  SizeEntry entries[kSizeEntries];  // 128 B
  if (!flash_.read(kSizeTableOffset, entries, sizeof entries)) return -1;
  for (int i = 0; i < kSizeEntries; ++i) {
    const SizeEntry& e = entries[i];
    if (e.ppem26_6 == ppem26_6 && e.pad[0] == 0 && e.pad[1] == 0 && e.pad[2] == 0) return i;
  }
  if (wipePending_) return -1;
  for (int i = 0; i < kSizeEntries; ++i) {
    const SizeEntry& e = entries[i];
    // Free only if every byte is erased: an entry cut short is never programmed again.
    if (e.ppem26_6 != kErased || e.flags != 0xFF || e.pad[0] != 0xFF || e.pad[1] != 0xFF || e.pad[2] != 0xFF) {
      continue;
    }
    const SizeEntry claim{ppem26_6, flags, {0, 0, 0}};  // the zero pad lands last and marks it in use
    if (!flash_.program(kSizeTableOffset + static_cast<uint32_t>(i) * sizeof(SizeEntry), &claim, sizeof claim)) {
      LOG_ERR("GST", "size table write failed; wipe pending");
      setWipePending();
      return -1;
    }
    return i;
  }
  LOG_INF("GST", "full: all %u sizes in use; wipe pending", static_cast<unsigned>(kSizeEntries));
  setWipePending();
  return -1;
}

// --- Kerning pairs ---

bool FlashGlyphStore::findPair(const uint16_t left, const uint16_t right, const uint8_t style,
                               int16_t* fontUnits) const {
  const uint32_t key = pairKey(left, right);
  if (!open_ || key == kErased) return false;
  const uint32_t slots = geometry_.pairSlots;
  uint32_t i = homeSlot(key, slots);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe, i = (i + 1 == slots) ? 0 : i + 1) {
    const Slot slot = loadSlot(pairs_, i);
    if (slotErased(slot)) return false;
    // The value's top bits are written as zero: a value cut short reads as some other style.
    if (slot.key != key || (slot.value >> 16) != style) continue;
    if (fontUnits) *fontUnits = static_cast<int16_t>(slot.value & 0xFFFF);
    return true;
  }
  return false;
}

bool FlashGlyphStore::storePair(const uint16_t left, const uint16_t right, const uint8_t style,
                                const int16_t fontUnits) {
  const uint32_t key = pairKey(left, right);
  if (!open_ || wipePending_ || key == kErased || style > 3) return false;
  // A full pair index only stops storing pairs; they come from the GPOS reader instead (never a wipe).
  if ((uint64_t{usedPairs_} + 1) * 100 > uint64_t{geometry_.pairSlots} * kIndexLoadPercent) return false;
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
  setWipePending();
  return false;
}

// --- Wipe ---

bool FlashGlyphStore::wipe() {
  if (!open_) return false;
  const Geometry g = geometry_;
  // Everything a lookup can reach goes first. The header, which holds the size table and the wipe word, is
  // rewritten last: a cut before that leaves wipe pending, and the next wipe() finishes the job.
  if (!eraseWritten(g.indexOffset, tableBytes(g.indexSlots)) || !eraseWritten(g.pairOffset, tableBytes(g.pairSlots)) ||
      !eraseWritten(g.logOffset, g.logSize)) {
    LOG_ERR("GST", "wipe: erase failed");
    return false;
  }
  logUsed_ = usedSlots_ = usedPairs_ = 0;
  auto header = makeUniqueNoThrow<Header>();
  if (!header) {
    LOG_ERR("GST", "wipe: no memory for the header (%u B)", static_cast<unsigned>(sizeof(Header)));
    return false;
  }
  if (!flash_.read(0, header.get(), sizeof(Header)) || !flash_.erase(0, kSectorBytes) ||
      !flash_.program(0, header.get(), sizeof(Header))) {
    LOG_ERR("GST", "wipe: header rewrite failed; the family will be installed again");
    close();
    return false;
  }
  wipePending_ = false;
  return true;
}
