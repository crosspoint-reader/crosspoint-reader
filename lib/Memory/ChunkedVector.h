#pragma once
// Append-only, non-relocating sequence for the build tables whose length the document decides:
// one entry per laid-out page (Section's page-offset LUT, the parser's paragraph LUT), so a book
// that is one XHTML file runs them to thousands of entries.
//
// std::vector is the wrong shape for those on this device. Past its reserve it doubles into one
// contiguous block and holds the old block while it copies, so past 512 pages the two LUTs ask
// for 8 KB and 4 KB at once, past 1024 for 16 KB and 8 KB -- in the middle of a parse that a
// borrowed build has already run down to a few KB contiguous. And it asks through the throwing
// operator new, which under -fno-exceptions is abort(): the reader reboots, the chapter rebuilds
// from page 0 on the next open and dies at the same page again.
//
// ChunkedVector stores the elements in chunks allocated nothrow (makeUniqueNoThrow):
//   - nothing is ever reallocated or copied, so peak use is live use;
//   - the first chunk holds what reserve() asked for (the build's page estimate, so a chapter
//     that fits it costs one block of the size a reserved vector took), or FirstChunk elements
//     without a reserve; later chunks start at 2 * FirstChunk and double up to MaxChunk, so the
//     largest block taken mid-parse is one MaxChunk chunk however long the chapter runs;
//   - push_back() returns false instead of aborting, so the caller can stop the build and keep
//     the pages it already has.
// Random access is O(1). The chunk directory is an inline array sized at compile time so that
// maxSize() >= Capacity whatever reserve() was given.
//
// Elements are value-initialised when their chunk is allocated and assigned on push_back, so T
// must be default-constructible. Each instantiation is its own code: keep their number small.
//
// Based on ChunkedVector from crosspoint-reader PR #3027 (Sidney San Martín, @s4y), reworked for
// this tree: a first chunk sized by reserve() so a chapter within the build's estimate takes the
// same single block as before, the directory derived from a required capacity, and pop_back(),
// clear() and moves for the parser's and the section's use.
#include <Memory.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#ifdef CHUNKED_VECTOR_FAULT_INJECTION
// Host tests only (firmware never defines it): how many more chunk allocations may succeed,
// across every ChunkedVector, before each one is refused. SIZE_MAX = never refuse. Drives the
// callers' out-of-memory paths, which an allocator with gigabytes behind it never reaches.
inline size_t& chunkedVectorAllocationsLeft() {
  static size_t left = SIZE_MAX;
  return left;
}
#endif

template <typename T, size_t FirstChunk, size_t MaxChunk, size_t Capacity>
class ChunkedVector {
  static_assert(FirstChunk > 0 && std::has_single_bit(FirstChunk));
  static_assert(MaxChunk >= FirstChunk && std::has_single_bit(MaxChunk));

  // The layout without a reserve: chunks [0, GROWTH_CHUNKS) hold FirstChunk << c elements, the
  // rest MaxChunk each. A reserve replaces chunk 0's FirstChunk with firstCapacity_; every later
  // chunk keeps its place in this layout (see locate).
  static constexpr size_t GROWTH_CHUNKS = std::bit_width(MaxChunk / FirstChunk) - 1;
  static constexpr size_t GROWTH_SIZE = FirstChunk * ((size_t{1} << GROWTH_CHUNKS) - 1);
  // Enough chunks for Capacity elements with the smallest first chunk reserve() allows.
  static constexpr size_t MAX_CHUNKS =
      GROWTH_CHUNKS + (Capacity > GROWTH_SIZE ? (Capacity - GROWTH_SIZE + MaxChunk - 1) / MaxChunk : 1);
  static constexpr size_t LAYOUT_SIZE = GROWTH_SIZE + (MAX_CHUNKS - GROWTH_CHUNKS) * MaxChunk;

  struct Location {
    size_t chunk;
    size_t offset;
  };
  // Position of element j in the reserve-free layout.
  static constexpr Location locateInLayout(const size_t j) {
    if (j < GROWTH_SIZE) {
      const size_t chunk = std::bit_width(j / FirstChunk + 1) - 1;
      return {chunk, j - FirstChunk * ((size_t{1} << chunk) - 1)};
    }
    const size_t k = j - GROWTH_SIZE;
    return {GROWTH_CHUNKS + k / MaxChunk, k % MaxChunk};
  }
  Location locate(const size_t i) const {
    if (i < firstCapacity_) return {0, i};
    // Past chunk 0: the same element of the reserve-free layout, whose chunk 0 is FirstChunk long.
    return locateInLayout(i - firstCapacity_ + FirstChunk);
  }
  size_t chunkCapacity(const size_t chunk) const {
    if (chunk == 0) return firstCapacity_;
    return chunk < GROWTH_CHUNKS ? FirstChunk << chunk : MaxChunk;
  }
  bool allocateChunk(const size_t chunk) {
#ifdef CHUNKED_VECTOR_FAULT_INJECTION
    if (chunkedVectorAllocationsLeft() == 0) return false;
    if (chunkedVectorAllocationsLeft() != SIZE_MAX) --chunkedVectorAllocationsLeft();
#endif
    chunks_[chunk] = makeUniqueNoThrow<T[]>(chunkCapacity(chunk));
    return chunks_[chunk] != nullptr;
  }

 public:
  ChunkedVector() = default;
  ChunkedVector(const ChunkedVector&) = delete;
  ChunkedVector& operator=(const ChunkedVector&) = delete;
  ChunkedVector(ChunkedVector&& other) noexcept { swap(other); }
  ChunkedVector& operator=(ChunkedVector&& other) noexcept {
    if (this != &other) {
      clear();
      swap(other);
    }
    return *this;
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  // The most elements this table can hold, given its first chunk.
  size_t maxSize() const { return LAYOUT_SIZE - FirstChunk + firstCapacity_; }
  // What maxSize() is at least, whatever the reserve.
  static constexpr size_t minMaxSize() { return LAYOUT_SIZE; }

  // Sizes the first chunk for `n` elements (at least FirstChunk) and allocates it now, while the
  // caller still has the heap it started from. Only before the first element: false when the
  // table already has one, or when the chunk could not be had -- the table then stays usable and
  // the first push_back allocates a FirstChunk chunk instead. n == 0 does nothing.
  bool reserve(const size_t n) {
    if (n == 0) return true;
    if (size_ != 0 || chunks_[0]) return false;
    firstCapacity_ = n < FirstChunk ? FirstChunk : n;
    if (allocateChunk(0)) return true;
    firstCapacity_ = FirstChunk;
    return false;
  }

  // False means the element was not stored: the table is at maxSize(), or its next chunk could
  // not be allocated. Never aborts.
  [[nodiscard]] bool push_back(T value) {
    const Location loc = locate(size_);
    if (loc.chunk >= MAX_CHUNKS) return false;
    if (!chunks_[loc.chunk] && !allocateChunk(loc.chunk)) return false;
    chunks_[loc.chunk][loc.offset] = std::move(value);
    size_++;
    return true;
  }
  // Drops the last element. Its chunk stays allocated for the next push_back.
  void pop_back() {
    if (size_ == 0) return;
    (*this)[size_ - 1] = T();
    size_--;
  }
  // Frees every chunk; the next reserve() or push_back() starts over.
  void clear() {
    for (auto& chunk : chunks_) chunk.reset();
    size_ = 0;
    firstCapacity_ = FirstChunk;
  }
  void swap(ChunkedVector& other) noexcept {
    for (size_t c = 0; c < MAX_CHUNKS; ++c) chunks_[c].swap(other.chunks_[c]);
    std::swap(size_, other.size_);
    std::swap(firstCapacity_, other.firstCapacity_);
  }

  T& operator[](const size_t i) {
    const Location loc = locate(i);
    return chunks_[loc.chunk][loc.offset];
  }
  const T& operator[](const size_t i) const {
    const Location loc = locate(i);
    return chunks_[loc.chunk][loc.offset];
  }
  T& back() { return (*this)[size_ - 1]; }
  const T& back() const { return (*this)[size_ - 1]; }

  class const_iterator {
   public:
    const_iterator(const ChunkedVector* owner, const size_t index) : owner_(owner), index_(index) {}
    const T& operator*() const { return (*owner_)[index_]; }
    const_iterator& operator++() {
      index_++;
      return *this;
    }
    bool operator!=(const const_iterator& other) const { return index_ != other.index_; }

   private:
    const ChunkedVector* owner_;
    size_t index_;
  };
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, size_); }

 private:
  std::unique_ptr<T[]> chunks_[MAX_CHUNKS];
  size_t size_ = 0;
  size_t firstCapacity_ = FirstChunk;
};
