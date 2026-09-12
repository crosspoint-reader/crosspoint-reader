#pragma once

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
// ChunkedVector instead allocates fixed-size chunks with nothrow new:
//   - no reallocation and no copying, so peak usage == live usage;
//   - the largest single allocation is one chunk, whatever the total length;
//   - push_back() reports failure so callers can degrade instead of aborting.
//
// Elements are value-initialized when a chunk is allocated and assigned on
// push_back, so T must be default-constructible. Random access is O(1). The
// chunk directory is a fixed inline array (MaxChunks pointers), which caps the
// total length at ChunkSize * MaxChunks -- push_back() returns false past that.
//
// Each instantiation emits its own code, so keep the number of distinct
// (T, ChunkSize, MaxChunks) combinations small.
template <typename T, size_t ChunkSize, size_t MaxChunks>
class ChunkedVector {
  static_assert(ChunkSize > 0 && MaxChunks > 0);

 public:
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  static constexpr size_t maxSize() { return ChunkSize * MaxChunks; }

  T& operator[](const size_t i) { return chunks_[i / ChunkSize][i % ChunkSize]; }
  const T& operator[](const size_t i) const { return chunks_[i / ChunkSize][i % ChunkSize]; }
  T& back() { return (*this)[size_ - 1]; }
  const T& back() const { return (*this)[size_ - 1]; }

  // False means the element was not stored: the directory is full, or a chunk
  // allocation failed. Never aborts.
  [[nodiscard]] bool push_back(T value) {
    const size_t chunk = size_ / ChunkSize;
    if (chunk >= MaxChunks) return false;
    if (!chunks_[chunk]) {
      chunks_[chunk].reset(new (std::nothrow) T[ChunkSize]());
      if (!chunks_[chunk]) return false;
    }
    chunks_[chunk][size_ % ChunkSize] = std::move(value);
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
