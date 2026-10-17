#pragma once

#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

// Append-only, non-relocating sequence for build-time tables whose length is
// driven by the document (the section page LUT, the chapter anchor map).
//
// std::vector is the wrong shape for these on the ESP32-C3. It grows
// geometrically into a single contiguous block and must hold the old and the new
// block at once while it copies, so a 1300-page section demands a ~24KB
// contiguous allocation on top of the ~12KB it is copying from -- at the exact
// moment the live parse is holding the heap down. When that allocation fails,
// std::vector calls the throwing operator new, which with -fno-exceptions is
// abort(): the device reboots mid-book. See EpubReaderActivity's build heap gate,
// which admits a build tick at a 16KB largest-free-block floor and so cannot
// possibly cover a 28KB doubling.
//
// ChunkedVector instead allocates chunks with nothrow new:
//   - no reallocation and no copying, so peak usage == live usage;
//   - chunks double from FirstChunk up to MaxChunk elements and stay there, so a
//     short table costs about what a std::vector would, and the largest single
//     allocation is one MaxChunk chunk whatever the total length;
//   - push_back() reports failure so callers can degrade instead of aborting.
//
// Elements are value-initialized when a chunk is allocated and assigned on
// push_back, so T must be default-constructible. Random access is O(1). The
// chunk directory is a fixed inline array (MaxChunks pointers), which caps the
// total length at maxSize() -- push_back() returns false past that.
//
// Each instantiation emits its own code, so keep the number of distinct
// template argument combinations small.
template <typename T, size_t FirstChunk, size_t MaxChunk, size_t MaxChunks>
class ChunkedVector {
  static_assert(FirstChunk > 0 && std::has_single_bit(FirstChunk));
  static_assert(MaxChunk >= FirstChunk && std::has_single_bit(MaxChunk));

  // Chunks [0, GROWTH_CHUNKS) hold FirstChunk << c elements; the rest hold MaxChunk.
  static constexpr size_t GROWTH_CHUNKS = std::bit_width(MaxChunk / FirstChunk) - 1;
  static constexpr size_t GROWTH_SIZE = FirstChunk * ((size_t{1} << GROWTH_CHUNKS) - 1);
  static_assert(MaxChunks > GROWTH_CHUNKS);

  static constexpr size_t chunkCapacity(const size_t chunk) {
    return chunk < GROWTH_CHUNKS ? FirstChunk << chunk : MaxChunk;
  }

  struct Location {
    size_t chunk;
    size_t offset;
  };
  static constexpr Location locate(const size_t i) {
    if (i < GROWTH_SIZE) {
      const size_t chunk = std::bit_width(i / FirstChunk + 1) - 1;
      return {chunk, i - FirstChunk * ((size_t{1} << chunk) - 1)};
    }
    const size_t j = i - GROWTH_SIZE;
    return {GROWTH_CHUNKS + j / MaxChunk, j % MaxChunk};
  }

 public:
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  static constexpr size_t maxSize() { return GROWTH_SIZE + (MaxChunks - GROWTH_CHUNKS) * MaxChunk; }

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

  // False means the element was not stored: the directory is full, or a chunk
  // allocation failed. Never aborts.
  [[nodiscard]] bool push_back(T value) {
    const Location loc = locate(size_);
    if (loc.chunk >= MaxChunks) return false;
    if (!chunks_[loc.chunk]) {
      chunks_[loc.chunk].reset(new (std::nothrow) T[chunkCapacity(loc.chunk)]());
      if (!chunks_[loc.chunk]) return false;
    }
    chunks_[loc.chunk][loc.offset] = std::move(value);
    size_++;
    return true;
  }

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
  std::unique_ptr<T[]> chunks_[MaxChunks];
  size_t size_ = 0;
};
