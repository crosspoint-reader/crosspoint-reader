#include "UiGlyphPool.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "UiGlyphCodec.h"

bool UiGlyphPool::init(size_t arenaBytes, uint16_t maxEntries) {
  release();
  initArenaBytes_ = arenaBytes;
  initMaxEntries_ = maxEntries;
  return reinit();
}

bool UiGlyphPool::reinit() {
  if (block_) return true;
  if (initArenaBytes_ == 0 || initMaxEntries_ == 0) return false;
  if (initArenaBytes_ > 0xFFFFu) return false;  // Entry::offset/length are uint16_t
  const size_t total = static_cast<size_t>(initMaxEntries_) * sizeof(Entry) + initArenaBytes_;
  block_ = makeUniqueNoThrow<uint8_t[]>(total);
  if (!block_) {
    LOG_ERR("UIGP", "OOM: %u bytes for glyph pool", static_cast<unsigned>(total));
    return false;
  }
  entries_ = reinterpret_cast<Entry*>(block_.get());
  arena_ = block_.get() + static_cast<size_t>(initMaxEntries_) * sizeof(Entry);
  maxEntries_ = initMaxEntries_;
  arenaBytes_ = initArenaBytes_;
  entryCount_ = 0;
  used_ = 0;
  stats_.storedBytes = 0;
  stats_.rawBytes = 0;
  LOG_DBG("UIGP", "Pool ready: %u entries, %u arena bytes", maxEntries_, static_cast<unsigned>(arenaBytes_));
  return true;
}

void UiGlyphPool::release() {
  block_.reset();
  entries_ = nullptr;
  arena_ = nullptr;
  entryCount_ = 0;
  maxEntries_ = 0;
  arenaBytes_ = 0;
  used_ = 0;
  kernPairCount_ = 0;
  stats_.storedBytes = 0;
  stats_.rawBytes = 0;
}

void UiGlyphPool::reset() {
  entryCount_ = 0;
  used_ = 0;
  kernPairCount_ = 0;
  stats_.storedBytes = 0;
  stats_.rawBytes = 0;
}

int32_t UiGlyphPool::lowerBound(uint32_t key) const {
  int32_t lo = 0, hi = entryCount_;
  while (lo < hi) {
    const int32_t mid = lo + (hi - lo) / 2;
    if ((entries_[mid].key & KEY_MASK) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

int32_t UiGlyphPool::find(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint) {
  if (!block_ || entryCount_ == 0) {
    stats_.misses++;
    return -1;
  }
  const uint32_t key = makeKey(instanceId, styleIdx, codepoint);
  const int32_t idx = lowerBound(key);
  if (idx < entryCount_ && (entries_[idx].key & KEY_MASK) == key) {
    entries_[idx].key |= FLAG_REF;
    stats_.hits++;
    return idx;
  }
  stats_.misses++;
  return -1;
}

int32_t UiGlyphPool::peek(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint) const {
  if (!block_ || entryCount_ == 0) return -1;
  const uint32_t key = makeKey(instanceId, styleIdx, codepoint);
  const int32_t idx = lowerBound(key);
  if (idx < entryCount_ && (entries_[idx].key & KEY_MASK) == key) return idx;
  return -1;
}

const UiGlyphPool::Metrics& UiGlyphPool::metricsOf(int32_t handle) const { return entries_[handle].metrics; }

bool UiGlyphPool::kernPairLookup(uint8_t instanceId, uint8_t styleIdx, uint8_t leftClass, uint8_t rightClass,
                                 int8_t* out) const {
  const uint32_t key = kernPairKey(instanceId, styleIdx, leftClass, rightClass);
  int32_t lo = 0, hi = kernPairCount_;
  while (lo < hi) {
    const int32_t mid = lo + (hi - lo) / 2;
    if ((kernPairs_[mid] >> 8) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < kernPairCount_ && (kernPairs_[lo] >> 8) == key) {
    if (out) *out = static_cast<int8_t>(kernPairs_[lo] & 0xFF);
    return true;
  }
  return false;
}

void UiGlyphPool::kernPairInsert(uint8_t instanceId, uint8_t styleIdx, uint8_t leftClass, uint8_t rightClass,
                                 int8_t value) {
  if (kernPairCount_ >= KERN_PAIR_CAP) return;  // drop-when-full
  const uint32_t key = kernPairKey(instanceId, styleIdx, leftClass, rightClass);
  int32_t lo = 0, hi = kernPairCount_;
  while (lo < hi) {
    const int32_t mid = lo + (hi - lo) / 2;
    if ((kernPairs_[mid] >> 8) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < kernPairCount_ && (kernPairs_[lo] >> 8) == key) return;  // already cached
  memmove(&kernPairs_[lo + 1], &kernPairs_[lo], (kernPairCount_ - lo) * sizeof(uint32_t));
  kernPairs_[lo] = (key << 8) | static_cast<uint8_t>(value);
  kernPairCount_++;
}

bool UiGlyphPool::isEmptyBitmap(int32_t handle) const { return entries_[handle].length == 0; }

bool UiGlyphPool::copyBitmap(int32_t handle, uint8_t* dst, uint16_t dstCapacity) const {
  if (!block_ || handle < 0 || handle >= entryCount_) return false;
  const Entry& e = entries_[handle];
  const uint16_t pixelCount = static_cast<uint16_t>(e.metrics.width) * e.metrics.height;
  const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(pixelCount);
  if (e.length == 0) return true;
  if (dstCapacity < rawLen) return false;
  if (e.key & FLAG_ENCODED) {
    UiGlyphCodec::decode(arena_ + e.offset, e.length, e.metrics.width, e.metrics.height, dst);
  } else {
    memcpy(dst, arena_ + e.offset, rawLen);
  }
  return true;
}

int32_t UiGlyphPool::entryAtOffset(uint16_t offset) const {
  for (int32_t i = 0; i < entryCount_; i++) {
    if (entries_[i].length > 0 && entries_[i].offset == offset) return i;
  }
  return -1;
}

void UiGlyphPool::removeEntry(int32_t idx) {
  Entry& e = entries_[idx];
  if (e.length > 0) {
    const uint16_t off = e.offset;
    const uint16_t len = e.length;
    memmove(arena_ + off, arena_ + off + len, used_ - off - len);
    for (int32_t i = 0; i < entryCount_; i++) {
      if (entries_[i].length > 0 && entries_[i].offset > off) {
        entries_[i].offset -= len;
      }
    }
    used_ -= len;
    stats_.storedBytes -= len;
    const uint16_t pixelCount = static_cast<uint16_t>(e.metrics.width) * e.metrics.height;
    stats_.rawBytes -= UiGlyphCodec::packed1BitBytes(pixelCount);
  }
  memmove(&entries_[idx], &entries_[idx + 1], (entryCount_ - idx - 1) * sizeof(Entry));
  entryCount_--;
}

bool UiGlyphPool::makeRoom(uint16_t need) {
  if (need > arenaBytes_) return false;

  // One index slot. Second chance applies here too, bounded to one full lap:
  // a rescue clears the ref bit without freeing a slot, so after at most
  // entryCount_ rescues an unreferenced victim exists.
  if (entryCount_ >= maxEntries_) {
    for (uint32_t lap = 0; entryCount_ >= maxEntries_; lap++) {
      int32_t victim = entryAtOffset(0);
      if (victim < 0) victim = 0;  // all entries are zero-length
      if ((entries_[victim].key & FLAG_REF) && lap < maxEntries_) {
        entries_[victim].key &= ~FLAG_REF;
        if (entries_[victim].length > 0) {
          // Rotate its bytes to the back so byte order keeps matching age order.
          const uint16_t len = entries_[victim].length;
          memcpy(entryScratch_, arena_, len);
          memmove(arena_, arena_ + len, used_ - len);
          for (int32_t i = 0; i < entryCount_; i++) {
            if (entries_[i].length > 0 && entries_[i].offset >= len) {
              entries_[i].offset -= len;
            }
          }
          entries_[victim].offset = static_cast<uint16_t>(used_ - len);
          memcpy(arena_ + used_ - len, entryScratch_, len);
        }
        stats_.rescues++;
        continue;
      }
      removeEntry(victim);
      stats_.evictions++;
    }
  }

  // Arena bytes.
  uint32_t laps = 0;
  while (used_ + need > arenaBytes_) {
    const int32_t victim = entryAtOffset(0);
    if (victim < 0) return false;  // nothing evictable holds bytes
    Entry& v = entries_[victim];
    if ((v.key & FLAG_REF) && laps++ < static_cast<uint32_t>(entryCount_)) {
      v.key &= ~FLAG_REF;
      const uint16_t len = v.length;
      memcpy(entryScratch_, arena_, len);
      memmove(arena_, arena_ + len, used_ - len);
      for (int32_t i = 0; i < entryCount_; i++) {
        if (entries_[i].length > 0 && entries_[i].offset >= len) {
          entries_[i].offset -= len;
        }
      }
      v.offset = static_cast<uint16_t>(used_ - len);
      memcpy(arena_ + used_ - len, entryScratch_, len);
      stats_.rescues++;
      continue;
    }
    removeEntry(victim);
    stats_.evictions++;
  }
  return true;
}

bool UiGlyphPool::insert(uint8_t instanceId, uint8_t styleIdx, uint32_t codepoint, const Metrics& metrics,
                         const uint8_t* srcBitmap, bool srcIs2Bit) {
  if (!block_) return false;

  const uint32_t key = makeKey(instanceId, styleIdx, codepoint);
  const int32_t at = lowerBound(key);
  if (at < entryCount_ && (entries_[at].key & KEY_MASK) == key) return true;  // already resident

  const uint16_t pixelCount = static_cast<uint16_t>(metrics.width) * metrics.height;
  const uint16_t rawLen = UiGlyphCodec::packed1BitBytes(pixelCount);
  if (rawLen > MAX_ENTRY_BYTES) return false;

  uint32_t flags = 0;
  uint16_t storeLen = 0;
  if (pixelCount > 0 && srcBitmap != nullptr) {
    // Accept the encoded form only when strictly smaller than raw 1-bit, so a
    // resident glyph never costs more than its converted bitmap.
    const uint16_t encLen =
        UiGlyphCodec::encode(srcBitmap, srcIs2Bit, metrics.width, metrics.height, encodeScratch_, rawLen - 1);
    if (encLen > 0) {
      storeLen = encLen;
      flags = FLAG_ENCODED;
    } else {
      UiGlyphCodec::convertTo1Bit(srcBitmap, srcIs2Bit, metrics.width, metrics.height, encodeScratch_);
      storeLen = rawLen;
    }
  }

  if (!makeRoom(storeLen)) return false;

  if (storeLen > 0) {
    memcpy(arena_ + used_, encodeScratch_, storeLen);
  }

  // makeRoom() may have removed entries; recompute the insertion point.
  const int32_t idx = lowerBound(key);
  memmove(&entries_[idx + 1], &entries_[idx], (entryCount_ - idx) * sizeof(Entry));
  // Born referenced: insertion is a use. Without this, a long fill would evict
  // the string's own earlier glyphs before the blit phase reads them.
  entries_[idx].key = key | flags | FLAG_REF;
  entries_[idx].offset = static_cast<uint16_t>(used_);
  entries_[idx].length = storeLen;
  entries_[idx].metrics = metrics;
  entryCount_++;
  used_ += storeLen;

  stats_.inserts++;
  stats_.storedBytes += storeLen;
  stats_.rawBytes += rawLen;
  return true;
}

void UiGlyphPool::logStats(const char* label) const {
  (void)label;  // unused when LOG_DBG is compiled out
  LOG_DBG("UIGP", "[%s] entries=%u/%u bytes=%u/%u (raw %u) hits=%lu misses=%lu ins=%lu evict=%lu rescue=%lu", label,
          entryCount_, maxEntries_, static_cast<unsigned>(used_), static_cast<unsigned>(arenaBytes_), stats_.rawBytes,
          static_cast<unsigned long>(stats_.hits), static_cast<unsigned long>(stats_.misses),
          static_cast<unsigned long>(stats_.inserts), static_cast<unsigned long>(stats_.evictions),
          static_cast<unsigned long>(stats_.rescues));
}
