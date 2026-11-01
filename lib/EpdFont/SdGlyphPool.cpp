#include "SdGlyphPool.h"

#include <FontPsram.h>
#include <HalMemory.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <new>

using freeink::font::psramDeleteArray;
using freeink::font::psramNewArray;

SdGlyphPool::SdGlyphPool() = default;
SdGlyphPool::~SdGlyphPool() { release(); }

// Sizes the pool at its first use: some boards add PSRAM to the heap after global
// constructors run, so the constructor cannot tell the board profiles apart.
bool SdGlyphPool::allocateIndex() {
  if (!chunkCap_) {
    chunkCap_ = HalMemory::getPsramHeap().totalBytes ? PSRAM_CHUNKS : INTERNAL_CHUNKS;
    entryCap_ = chunkCap_ * CHUNK_BYTES / 72;
    indexCapacity_ = 2;
    while (indexCapacity_ < entryCap_ * 2) indexCapacity_ *= 2;
  }
  index_ = psramNewArray<uint16_t>(indexCapacity_);
  if (!index_) {
    LOG_ERR("SDCF", "Failed to allocate pool index (%u bytes)", indexCapacity_ * 2);
    return false;
  }
  std::fill_n(index_, indexCapacity_, NO_ENTRY);
  return true;
}

uint8_t SdGlyphPool::registerFont() {
  for (uint8_t slot = 0; slot < 64; ++slot) {
    const uint64_t bit = uint64_t{1} << slot;
    if (!(fonts_ & bit)) {
      fonts_ |= bit;
      return slot;
    }
  }
  LOG_ERR("SDCF", "SD glyph pool font slots exhausted");
  return NO_FONT;
}

void SdGlyphPool::unregisterFont(const uint8_t slot) {
  if (slot == NO_FONT) return;
  dropFont(slot);
  fonts_ &= ~(uint64_t{1} << slot);
  if (!fonts_) release();
}

SdGlyphPool::Entry* SdGlyphPool::entry(const uint16_t encoded) const {
  return reinterpret_cast<Entry*>(chunks_[encoded / (CHUNK_BYTES / 4)].data + (encoded % (CHUNK_BYTES / 4)) * 4);
}

uint16_t SdGlyphPool::indexSlot(const uint8_t fontStyle, const uint32_t codepoint) const {
  uint32_t hash = codepoint * 0x9E3779B1u + fontStyle * 0x85EBCA77u;
  hash ^= hash >> 16;
  uint16_t slot = hash & (indexCapacity_ - 1);
  while (index_[slot] != NO_ENTRY) {
    const auto* found = entry(index_[slot]);
    if (found->codepoint == codepoint && found->fontStyle == fontStyle) break;
    slot = (slot + 1) & (indexCapacity_ - 1);
  }
  return slot;
}

void SdGlyphPool::rebuildIndex() {
  entryCount_ = 0;
  if (!index_) return;
  std::fill_n(index_, indexCapacity_, NO_ENTRY);
  for (uint8_t i = 0; i < chunkCount_; ++i) {
    const uint8_t id = order_[i];
    const auto& chunk = chunks_[id];
    for (uint16_t offset = 0; offset < chunk.used;) {
      const auto* item = reinterpret_cast<const Entry*>(chunk.data + offset);
      if (item->codepoint != DROPPED) {
        index_[indexSlot(item->fontStyle, item->codepoint)] = id * (CHUNK_BYTES / 4) + offset / 4;
        ++entryCount_;
      }
      offset += entryBytes(item->glyph);
    }
  }
}

bool SdGlyphPool::pinned(const uint8_t id) const {
  const auto& chunk = chunks_[id];
  for (uint16_t offset = 0; offset < chunk.used;) {
    const auto* item = reinterpret_cast<const Entry*>(chunk.data + offset);
    if (item->codepoint != DROPPED && item->generation == generation_) return true;
    offset += entryBytes(item->glyph);
  }
  return false;
}

void SdGlyphPool::advanceGeneration() {
  if (++generation_) return;
  generation_ = 1;
  for (uint16_t i = 0; index_ && i < indexCapacity_; ++i) {
    if (index_[i] != NO_ENTRY) entry(index_[i])->generation = 0;
  }
}

void SdGlyphPool::beginScope() {
  clearCache();
  ++scopeDepth_;
}
void SdGlyphPool::endScope() {
  if (scopeDepth_) --scopeDepth_;
  clearCache();
}
void SdGlyphPool::beginPrewarm() {
  if (!scopeDepth_) advanceGeneration();
}

void SdGlyphPool::clearCache() {
  advanceGeneration();
  psramDeleteArray(overflow_);
  overflow_ = nullptr;
  overflowCapacity_ = overflowHead_ = overflowCount_ = 0;
  if (HalMemory::getInternalHeap().freeBytes < RETAIN_MIN_FREE_HEAP) trimUnpinned();
}

bool SdGlyphPool::trimUnpinned() {
  bool freed = false;
  for (uint8_t i = 0; i < chunkCount_;) {
    const uint8_t id = order_[i];
    if (pinned(id)) {
      ++i;
      continue;
    }
    psramDeleteArray(chunks_[id].data);
    chunks_[id] = {};
    std::move(order_ + i + 1, order_ + chunkCount_, order_ + i);
    --chunkCount_;
    freed = true;
  }
  if (!freed) return false;
  rebuildIndex();
  // Overflow entries may be in use by the caller that needed room; only a cache boundary frees them.
  if (!chunkCount_) freeIndexAndScratch();
  return true;
}

void SdGlyphPool::freeIndexAndScratch() {
  psramDeleteArray(index_);
  index_ = nullptr;
  if (scratch_.capacity) scratchHint_ = scratch_.capacity;
  psramDeleteArray(reinterpret_cast<uint8_t*>(scratch_.codepoints));
  scratch_ = {};
}

void SdGlyphPool::freeAuxiliary() {
  freeIndexAndScratch();
  psramDeleteArray(overflow_);
  overflow_ = nullptr;
  overflowCapacity_ = overflowHead_ = overflowCount_ = 0;
}

void SdGlyphPool::release() {
  for (auto& chunk : chunks_) {
    psramDeleteArray(chunk.data);
    chunk = {};
  }
  chunkCount_ = entryCount_ = 0;
  freeAuxiliary();
}

void SdGlyphPool::dropFont(const uint8_t slot) {
  if (slot == NO_FONT) return;
  for (uint8_t i = 0; i < chunkCount_;) {
    const uint8_t id = order_[i];
    auto& chunk = chunks_[id];
    bool live = false;
    for (uint16_t offset = 0; offset < chunk.used;) {
      auto* item = reinterpret_cast<Entry*>(chunk.data + offset);
      if ((item->fontStyle >> 2) == slot) item->codepoint = DROPPED;
      live |= item->codepoint != DROPPED;
      offset += entryBytes(item->glyph);
    }
    if (live) {
      ++i;
    } else {
      psramDeleteArray(chunk.data);
      chunk = {};
      std::move(order_ + i + 1, order_ + chunkCount_, order_ + i);
      --chunkCount_;
    }
  }
  for (uint8_t i = 0; i < overflowCount_;) {
    const auto* item = reinterpret_cast<const Entry*>(overflow_ + overflowOffsets_[i]);
    if ((item->fontStyle >> 2) != slot) {
      ++i;
      continue;
    }
    std::move(overflowOffsets_ + i + 1, overflowOffsets_ + overflowCount_, overflowOffsets_ + i);
    --overflowCount_;
  }
  rebuildIndex();
  if (!chunkCount_) freeAuxiliary();
}

SdGlyphPool::Scratch* SdGlyphPool::scratch(const uint16_t needed) {
  if (needed > MAX_PAGE_GLYPHS) return nullptr;
  if (needed > scratch_.capacity) {
    uint16_t minimum = 32;
    while (minimum < needed) minimum *= 2;
    uint16_t capacity = std::max(minimum, scratchHint_);
    size_t bytes = capacity * (sizeof(uint32_t) + sizeof(Entry*));
    auto* grown = psramNewArray<uint8_t>(bytes);
    if (!grown && capacity > minimum) {
      // The remembered capacity did not fit a tight heap; the smaller buffer may still.
      capacity = minimum;
      bytes = capacity * (sizeof(uint32_t) + sizeof(Entry*));
      grown = psramNewArray<uint8_t>(bytes);
    }
    if (!grown) {
      LOG_ERR("SDCF", "Failed to allocate pool scratch (%u bytes)", unsigned(bytes));
      return nullptr;
    }
    if (scratch_.capacity) memcpy(grown, scratch_.codepoints, scratch_.capacity * sizeof(uint32_t));
    psramDeleteArray(reinterpret_cast<uint8_t*>(scratch_.codepoints));
    scratch_ = {reinterpret_cast<uint32_t*>(grown), reinterpret_cast<Entry**>(grown + capacity * sizeof(uint32_t)),
                capacity};
  }
  return scratch_.capacity ? &scratch_ : nullptr;
}

bool SdGlyphPool::addChunk() {
  if (!index_ && !allocateIndex()) return false;
  if (chunkCount_ >= chunkCap_) return false;
  uint8_t id = 0;
  while (chunks_[id].data) ++id;
  auto* data = psramNewArray<uint8_t>(CHUNK_BYTES);
  if (!data) {
    LOG_ERR("SDCF", "Failed to allocate glyph chunk (%u bytes)", CHUNK_BYTES);
    if (!chunkCount_) {
      psramDeleteArray(index_);
      index_ = nullptr;
    }
    return false;
  }
  chunks_[id] = {data, 0};
  order_[chunkCount_++] = id;
  return true;
}

void SdGlyphPool::makeNewest(const uint8_t orderIndex, const uint16_t used) {
  const uint8_t id = order_[orderIndex];
  chunks_[id].used = used;
  std::move(order_ + orderIndex + 1, order_ + chunkCount_, order_ + orderIndex);
  order_[chunkCount_ - 1] = id;
  rebuildIndex();
}

// Prewarm may move survivors within one chunk; drawing never calls this path.
bool SdGlyphPool::compactForPrewarm(const uint8_t orderIndex, const uint32_t bytes) {
  auto& chunk = chunks_[order_[orderIndex]];
  uint16_t retained = 0;
  for (uint16_t offset = 0; offset < chunk.used;) {
    const auto* item = reinterpret_cast<const Entry*>(chunk.data + offset);
    const uint32_t size = entryBytes(item->glyph);
    if (item->codepoint != DROPPED && item->generation == generation_) retained += size;
    offset += size;
  }
  if (retained == chunk.used || retained + bytes > CHUNK_BYTES) return false;
  uint16_t write = 0;
  for (uint16_t read = 0; read < chunk.used;) {
    const auto* item = reinterpret_cast<const Entry*>(chunk.data + read);
    const uint32_t size = entryBytes(item->glyph);
    if (item->codepoint != DROPPED && item->generation == generation_) {
      memmove(chunk.data + write, item, size);
      write += size;
    }
    read += size;
  }
  makeNewest(orderIndex, write);
  return true;
}

SdGlyphPool::Entry* SdGlyphPool::find(const uint8_t fontStyle, const uint32_t codepoint) {
  if (index_) {
    const uint16_t encoded = index_[indexSlot(fontStyle, codepoint)];
    if (encoded != NO_ENTRY) {
      auto* found = entry(encoded);
      found->generation = generation_;
      return found;
    }
  }
  for (uint8_t i = 0; i < overflowCount_; ++i) {
    auto* found = reinterpret_cast<Entry*>(overflow_ + overflowOffsets_[i]);
    if (found->fontStyle == fontStyle && found->codepoint == codepoint) return found;
  }
  return nullptr;
}

SdGlyphPool::Entry* SdGlyphPool::insert(const uint8_t fontStyle, const uint32_t codepoint, const EpdGlyph& glyph,
                                        const bool prewarm) {
  const uint32_t bytes = entryBytes(glyph);
  if (bytes > CHUNK_BYTES) return nullptr;
  if (!chunkCount_ && !addChunk()) return nullptr;
  if (chunks_[order_[chunkCount_ - 1]].used + bytes > CHUNK_BYTES || entryCount_ >= entryCap_) {
    uint8_t reusable = 0;
    while (reusable < chunkCount_ && pinned(order_[reusable])) ++reusable;
    if (reusable < chunkCount_) {
      makeNewest(reusable);
    } else {
      bool compacted = false;
      if (prewarm) {
        for (uint8_t i = 0; i < chunkCount_; ++i) {
          if (compactForPrewarm(i, bytes)) {
            compacted = true;
            break;
          }
        }
      }
      if (!compacted && (entryCount_ >= entryCap_ || !addChunk())) return nullptr;
    }
    // Compaction can reclaim only dropped bytes and leave the live count at the cap.
    if (entryCount_ >= entryCap_) return nullptr;
  }
  const uint8_t id = order_[chunkCount_ - 1];
  auto& chunk = chunks_[id];
  auto* item = new (chunk.data + chunk.used) Entry{codepoint, glyph, generation_, fontStyle, glyph.dataLength == 0};
  index_[indexSlot(fontStyle, codepoint)] = id * (CHUNK_BYTES / 4) + chunk.used / 4;
  chunk.used += bytes;
  ++entryCount_;
  return item;
}

bool SdGlyphPool::isOverflow(const Entry* item) const {
  const auto* address = reinterpret_cast<const uint8_t*>(item);
  return overflow_ && address >= overflow_ && address < overflow_ + overflowCapacity_;
}

SdGlyphPool::Entry* SdGlyphPool::insertOverflow(const uint8_t fontStyle, const uint32_t codepoint,
                                                const EpdGlyph& glyph) {
  const uint32_t bytes = entryBytes(glyph);
  if (bytes > overflowCapacity_) {
    psramDeleteArray(overflow_);
    overflow_ = nullptr;
    overflowCount_ = overflowHead_ = overflowCapacity_ = 0;
    uint32_t capacity = std::max<uint32_t>(CHUNK_BYTES, bytes);
    overflow_ = psramNewArray<uint8_t>(capacity);
    if (!overflow_ && capacity > bytes) {
      // A fragmented heap may still hold this one glyph.
      capacity = bytes;
      overflow_ = psramNewArray<uint8_t>(capacity);
    }
    if (!overflow_) {
      LOG_ERR("SDCF", "Failed to allocate glyph overflow (%u bytes)", capacity);
      return nullptr;
    }
    overflowCapacity_ = capacity;
  }
  if (overflowHead_ + bytes > overflowCapacity_) overflowHead_ = 0;
  // Drop FIFO entries until neither the slot limit nor overlapping bytes block the insertion.
  uint8_t drop = overflowCount_ == OVERFLOW_ENTRIES ? 1 : 0;
  for (uint8_t i = 0; i < overflowCount_; ++i) {
    const uint32_t start = overflowOffsets_[i];
    const auto* item = reinterpret_cast<const Entry*>(overflow_ + start);
    if (start < overflowHead_ + bytes && start + entryBytes(item->glyph) > overflowHead_) drop = i + 1;
  }
  std::move(overflowOffsets_ + drop, overflowOffsets_ + overflowCount_, overflowOffsets_);
  overflowCount_ -= drop;
  auto* item = new (overflow_ + overflowHead_) Entry{codepoint, glyph, generation_, fontStyle, glyph.dataLength == 0};
  overflowOffsets_[overflowCount_++] = overflowHead_;
  overflowHead_ += bytes;
  return item;
}
