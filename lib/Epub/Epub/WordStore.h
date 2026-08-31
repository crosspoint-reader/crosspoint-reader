#pragma once

#include <Memory.h>

#include <cstdint>
#include <cstring>
#include <string_view>

// Bump arena for layout-time word text. Words are copied in NUL-terminated so
// views can feed C string APIs (drawText, startsWithRtl) directly.
//
// Text accumulates into fixed 2KB chunks; a chunk is freed once every word in
// it has been released. Words are consumed FIFO as lines are extracted, so
// chunks retire in step with layout even when one paragraph spans a whole
// chapter. Uniform 2KB requests are deliberate: they stay allocatable under
// the fragmentation that defeats per-word variable-size allocations.
//
// Every allocation is nothrow. append() reports failure instead of aborting
// (-fno-exceptions turns a failed implicit allocation into abort()), giving
// the layout path a single detectable OOM choke point.
class WordStore {
 public:
  struct StoredWord {
    uint32_t chunk = 0;
    uint16_t off = 0;
    uint16_t len = 0;
  };

  // Copies [text, text+len) plus a NUL terminator into the arena.
  // Returns false on OOM; out is untouched in that case.
  bool append(const char* text, size_t len, StoredWord& out);

  // NUL-terminated view of a stored word.
  std::string_view view(const StoredWord& w) const { return {charsAt(w), w.len}; }
  const char* cstr(const StoredWord& w) const { return charsAt(w); }

  // A suffix of an existing word (shares its bytes and trailing NUL). The
  // suffix inherits the original's release obligation: release exactly one of
  // the two, never both.
  static StoredWord suffix(const StoredWord& w, size_t byteOffset) {
    return {w.chunk, static_cast<uint16_t>(w.off + byteOffset), static_cast<uint16_t>(w.len - byteOffset)};
  }

  // Declares a stored word consumed. Frees its chunk once no other live word
  // points into it.
  void release(const StoredWord& w);

  // Chunk enumeration for whole-paragraph codepoint scans (SD font advance
  // prewarm). Chunks hold consecutive NUL-terminated words; data may be null
  // for retired chunks.
  size_t chunkCount() const { return chunkCount_; }
  const char* chunkData(size_t i) const { return chunks_[i].data.get(); }
  size_t chunkUsed(size_t i) const { return chunks_[i].used; }

 private:
  struct Chunk {
    std::unique_ptr<char[]> data;
    uint16_t capacity = 0;
    uint16_t used = 0;
    uint16_t live = 0;
  };

  const char* charsAt(const StoredWord& w) const { return chunks_[w.chunk].data.get() + w.off; }
  bool ensureChunkSlot();

  static constexpr size_t CHUNK_SIZE = 2048;
  // The uint16_t off/used fields force this bound; parser tokens are capped
  // far below it (MAX_WORD_SIZE), so truncation is a theoretical safety net.
  static constexpr size_t MAX_WORD_BYTES = 0xFFFE;

  std::unique_ptr<Chunk[]> chunks_;
  size_t chunkCount_ = 0;
  size_t chunkCapacity_ = 0;
};
