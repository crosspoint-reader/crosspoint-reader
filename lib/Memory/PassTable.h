#pragma once
// A table for resolving many keys a table-full per pass over some file, sized to the memory there
// is instead of all or nothing.
//
// The first-open index builds resolve one name per spine item against a file far larger than RAM
// (the ZIP central directory, the OPF's item store). A table for every spine item at once is 16 B
// an item, 28 KB for King's Avatar's 1732 -- and when that did not fit, the code fell back to one
// pass over the file per item: 427 s of a 431 s first open on the X3 (2026-10-20). A table of any
// size turns that into ceil(n / capacity) passes, so it takes what there is:
//   fromRegion  a block of a lent region (the secondary framebuffer during first-open indexing),
//               leaving `keepFree` of it for whoever reserves after it; released on destruction;
//   fromHeap    the heap's largest block, leaving `reserveBytes` of the free heap for the rest of
//               the build, at least `minEntries` whatever the heap reports, halving on a refusal
//               down to `floorEntries`. Allocated nothrow (the device is -fno-exceptions).
// Either can carry `extraBytes` of scratch beside the table (a read window): from the same block
// in a region, ahead of the table on the heap so the table's sizing sees it.
//
// Empty (capacity() == 0) when neither found room: the caller resolves its keys one by one.
// Nothing here outlives the table, and nothing about the keys is kept: per-item data that must
// persist belongs on the SD card.
#include <BuildArena.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

template <typename Entry>
class PassTable {
 public:
  struct HeapSizing {
    size_t minEntries = 128;
    size_t floorEntries = 16;
    size_t reserveBytes = 16 * 1024;
  };
  // A heap block a little larger than the table: the allocator's header and alignment.
  static constexpr size_t HEAP_BLOCK_SLACK = 64;

  PassTable() = default;
  PassTable(PassTable&& other) noexcept { *this = std::move(other); }
  PassTable& operator=(PassTable&& other) noexcept {
    if (this != &other) {
      releaseRegion();
      region_ = other.region_;
      block_ = std::move(other.block_);
      heapExtra_ = std::move(other.heapExtra_);
      heapTable_ = std::move(other.heapTable_);
      table_ = other.table_;
      extra_ = other.extra_;
      extraBytes_ = other.extraBytes_;
      capacity_ = other.capacity_;
      other.region_ = nullptr;
      other.table_ = nullptr;
      other.extra_ = nullptr;
      other.extraBytes_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }
  PassTable(const PassTable&) = delete;
  PassTable& operator=(const PassTable&) = delete;
  ~PassTable() { releaseRegion(); }

  // Up to `wanted` entries in a block of `region`, with `keepFree` bytes of it left over; empty
  // when that leaves room for fewer than min(wanted, minEntries).
  static PassTable fromRegion(BuildArena* region, const size_t wanted, const size_t minEntries,
                              const size_t extraBytes = 0, const size_t keepFree = 0) {
    PassTable t;
    if (region == nullptr || !region->valid() || wanted == 0) return t;
    const size_t room = region->capacity() - region->used();
    const size_t overhead = keepFree + extraBytes + 2 * alignof(std::max_align_t);
    if (room <= overhead) return t;
    const size_t fit = std::min(wanted, (room - overhead) / sizeof(Entry));
    if (fit == 0 || fit < std::min(wanted, minEntries)) return t;
    t.block_ = region->reserveBlock();
    t.table_ = region->allocArray<Entry>(fit);
    if (t.table_ == nullptr) {
      region->release(t.block_);
      return t;
    }
    t.region_ = region;
    t.capacity_ = fit;
    if (extraBytes > 0) {
      t.extra_ = static_cast<uint8_t*>(region->alloc(extraBytes));
      t.extraBytes_ = t.extra_ != nullptr ? extraBytes : 0;
    }
    return t;
  }

  // Up to `wanted` entries from the heap, sized as the class comment says.
  static PassTable fromHeap(const size_t wanted, const HeapSizing& sizing, const size_t extraBytes = 0) {
    PassTable t;
    if (wanted == 0) return t;
    if (extraBytes > 0) {
      t.heapExtra_ = makeUniqueNoThrow<uint8_t[]>(extraBytes);
      t.extra_ = t.heapExtra_.get();
      t.extraBytes_ = t.extra_ != nullptr ? extraBytes : 0;
    }
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
    const size_t budget = std::min(largest > HEAP_BLOCK_SLACK ? largest - HEAP_BLOCK_SLACK : 0,
                                   freeHeap > sizing.reserveBytes ? freeHeap - sizing.reserveBytes : 0);
    const size_t floor = std::max<size_t>(1, std::min(wanted, sizing.floorEntries));
    size_t want = std::clamp(budget / sizeof(Entry), std::min(wanted, sizing.minEntries), wanted);
    for (; want >= floor; want /= 2) {
      t.heapTable_ = makeUniqueNoThrow<Entry[]>(want);
      if (t.heapTable_) {
        t.table_ = t.heapTable_.get();
        t.capacity_ = want;
        return t;
      }
    }
    return t;
  }

  Entry* entries() const { return table_; }
  size_t capacity() const { return capacity_; }
  uint8_t* extra() const { return extra_; }
  size_t extraBytes() const { return extraBytes_; }
  bool inRegion() const { return region_ != nullptr; }

 private:
  void releaseRegion() {
    if (region_ != nullptr && block_.valid()) region_->release(block_);
    region_ = nullptr;
  }

  BuildArena* region_ = nullptr;
  BuildArena::Block block_;
  std::unique_ptr<uint8_t[]> heapExtra_;
  std::unique_ptr<Entry[]> heapTable_;
  Entry* table_ = nullptr;
  uint8_t* extra_ = nullptr;
  size_t extraBytes_ = 0;
  size_t capacity_ = 0;
};
