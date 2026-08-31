#include "WordStore.h"

#include <Logging.h>
#include <MemoryManager.h>

namespace {
// Extra bytes beyond the failed request when asking the SDK memory manager to
// evict caches: leaves room for the parallel-array growth and TextBlock arena
// that immediately follow a chunk allocation, instead of evicting to an exact
// fit and failing on the very next allocation.
constexpr size_t RECLAIM_HEADROOM = 8 * 1024;

// Nothrow allocation with one evict-and-retry pass. ensureFree() asks the
// registered cache sinks (SD-font mini data, render glyph cache) to release
// rebuildable memory; only when even that cannot satisfy the request does the
// caller's drop/fail path run.
std::unique_ptr<char[]> allocWithReclaim(const size_t bytes) {
  auto data = makeUniqueNoThrow<char[]>(bytes);
  if (data) return data;
  freeink::MemoryManager::instance().ensureFree(bytes + RECLAIM_HEADROOM);
  data = makeUniqueNoThrow<char[]>(bytes);
  if (data) {
    LOG_DBG("WST", "Cache eviction rescued a %u-byte chunk allocation", static_cast<unsigned>(bytes));
  }
  return data;
}
}  // namespace

bool WordStore::ensureChunkSlot() {
  if (chunkCount_ < chunkCapacity_) return true;
  // Manual nothrow growth: std::vector would abort on OOM under -fno-exceptions.
  const size_t newCapacity = chunkCapacity_ == 0 ? 8 : chunkCapacity_ * 2;
  auto grown = makeUniqueNoThrow<Chunk[]>(newCapacity);
  if (!grown) {
    freeink::MemoryManager::instance().ensureFree(newCapacity * sizeof(Chunk) + RECLAIM_HEADROOM);
    grown = makeUniqueNoThrow<Chunk[]>(newCapacity);
    if (!grown) return false;
  }
  for (size_t i = 0; i < chunkCount_; i++) {
    grown[i] = std::move(chunks_[i]);
  }
  chunks_ = std::move(grown);
  chunkCapacity_ = newCapacity;
  return true;
}

bool WordStore::append(const char* text, size_t len, StoredWord& out) {
  if (len > MAX_WORD_BYTES) len = MAX_WORD_BYTES;
  const size_t need = len + 1;

  Chunk* target = nullptr;
  if (chunkCount_ > 0) {
    Chunk& last = chunks_[chunkCount_ - 1];
    if (last.data && static_cast<size_t>(last.capacity) - last.used >= need) target = &last;
  }
  if (!target) {
    if (!ensureChunkSlot()) return false;
    // Words larger than a chunk get a dedicated exact-fit chunk so the offset
    // arithmetic stays uniform; everything else shares 2KB chunks.
    const size_t cap = need > CHUNK_SIZE ? need : CHUNK_SIZE;
    auto data = allocWithReclaim(cap);
    if (!data) return false;
    Chunk& fresh = chunks_[chunkCount_];
    fresh.data = std::move(data);
    fresh.capacity = static_cast<uint16_t>(cap);
    fresh.used = 0;
    fresh.live = 0;
    chunkCount_++;
    target = &fresh;
  }

  out.chunk = static_cast<uint32_t>(target - chunks_.get());
  out.off = target->used;
  out.len = static_cast<uint16_t>(len);
  memcpy(target->data.get() + target->used, text, len);
  target->data[target->used + len] = '\0';
  target->used = static_cast<uint16_t>(target->used + need);
  target->live++;
  return true;
}

void WordStore::release(const StoredWord& w) {
  Chunk& c = chunks_[w.chunk];
  if (c.live > 0) c.live--;
  // The tail chunk is still accepting appends; keep it even when drained.
  if (c.live == 0 && c.data && w.chunk + 1 != chunkCount_) {
    c.data.reset();
  }
}
