#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "NorFlash.h"

/// NOR flash for host tests.
///
/// - program() can only clear bits. A write that needs a 0 -> 1 change is counted in violations(); the bit
///   stays 0, as on the chip. Programming a byte that is no longer erased with anything but 0xFF is counted
///   in reprograms(): the store never does it. Store tests assert both are 0.
/// - erase() works in whole 4 KB sectors and counts erases per sector (the wear tests).
/// - Power cuts: after cutAfterSteps(n), n more steps succeed (one step = one programmed byte or one
///   erased sector). The operation in flight then stops part-way, and every later program or erase fails
///   until powerCycle(). With partialCut(true), the byte being programmed at the cut keeps some of its 1
///   bits, as a page program interrupted mid-way can. A sector being erased at the cut is left half
///   erased. Reads keep working, so a test can inspect the chip.
/// - map() returns a live view of the memory: later programs and erases show through it, as ESP-IDF keeps
///   a mapping coherent after a write. Mappings are limited to freePages MMU pages.
class SimNorFlash final : public NorFlash {
 public:
  explicit SimNorFlash(const uint32_t bytes, const uint32_t freePages = 32)
      : mem_(bytes, 0xFF), sectorErases_(bytes / kSectorBytes, 0), freePages_(freePages) {}

  uint32_t size() const override { return static_cast<uint32_t>(mem_.size()); }

  bool read(const uint32_t offset, void* dst, const uint32_t len) const override {
    if (!inRange(offset, len)) return false;
    std::memcpy(dst, mem_.data() + offset, len);
    return true;
  }

  bool program(const uint32_t offset, const void* src, const uint32_t len) override {
    if (dead_ || !inRange(offset, len)) return false;
    if (failProgram_) {
      if (failSkip_ == 0) {
        failProgram_ = false;
        return false;
      }
      --failSkip_;
    }
    ++programCalls_;
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < len; ++i) {
      uint8_t& cell = mem_[offset + i];
      if (!takeStep()) {
        if (partialCut_) cell &= static_cast<uint8_t>(bytes[i] | 0x5A);  // only some of its 0 bits land
        return false;
      }
      if ((cell & bytes[i]) != bytes[i]) ++violations_;
      if (cell != 0xFF && bytes[i] != 0xFF) ++reprograms_;
      cell &= bytes[i];
    }
    return true;
  }

  bool erase(const uint32_t offset, const uint32_t len) override {
    if (dead_ || !inRange(offset, len) || offset % kSectorBytes != 0 || len % kSectorBytes != 0) return false;
    ++eraseCalls_;
    for (uint32_t sector = offset; sector < offset + len; sector += kSectorBytes) {
      if (!takeStep()) {
        std::fill_n(mem_.begin() + sector, kSectorBytes / 2, 0xFF);  // the cut leaves it half erased
        return false;
      }
      std::fill_n(mem_.begin() + sector, kSectorBytes, 0xFF);
      ++sectorErases_[sector / kSectorBytes];
    }
    return true;
  }

  const uint8_t* map(const uint32_t offset, const uint32_t len) override {
    if (len == 0 || !inRange(offset, len)) return nullptr;
    const uint32_t pages = (offset % kMapPageBytes + len + kMapPageBytes - 1) / kMapPageBytes;
    if (mappedPages_ + pages > freePages_) return nullptr;
    mappedPages_ += pages;
    maps_.push_back({mem_.data() + offset, pages});
    return mem_.data() + offset;
  }

  void unmap(const uint8_t* mapped) override {
    for (auto it = maps_.begin(); it != maps_.end(); ++it) {
      if (it->ptr == mapped) {
        mappedPages_ -= it->pages;
        maps_.erase(it);
        return;
      }
    }
  }

  uint32_t freeMapPages() const override { return freePages_ - mappedPages_; }

  // --- test controls ---
  void cutAfterSteps(const int64_t steps) { budget_ = steps; }
  void partialCut(const bool on) { partialCut_ = on; }
  /// A reboot: writes work again and every mapping is gone.
  void powerCycle() {
    dead_ = false;
    budget_ = -1;
    maps_.clear();
    mappedPages_ = 0;
  }
  /// Fails one program() call (writing nothing): the next one, or the one after `skip` successful calls.
  void failNextProgram(const uint32_t skip = 0) {
    failProgram_ = true;
    failSkip_ = skip;
  }
  void setFreePages(const uint32_t pages) { freePages_ = pages; }

  // --- what happened ---
  bool dead() const { return dead_; }
  uint64_t steps() const { return steps_; }
  uint32_t violations() const { return violations_; }
  uint32_t reprograms() const { return reprograms_; }
  uint32_t programCalls() const { return programCalls_; }
  uint32_t eraseCalls() const { return eraseCalls_; }
  uint32_t sectorErases(const uint32_t sector) const { return sectorErases_.at(sector); }
  uint32_t totalSectorErases() const {
    uint32_t total = 0;
    for (const uint32_t n : sectorErases_) total += n;
    return total;
  }
  uint32_t mappings() const { return static_cast<uint32_t>(maps_.size()); }
  uint8_t* raw() { return mem_.data(); }

 private:
  struct Mapping {
    const uint8_t* ptr;
    uint32_t pages;
  };

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return offset <= mem_.size() && len <= mem_.size() - offset;
  }

  // One step of work; false when the power fails on it.
  bool takeStep() {
    if (budget_ == 0) {
      dead_ = true;
      return false;
    }
    if (budget_ > 0) --budget_;
    ++steps_;
    return true;
  }

  std::vector<uint8_t> mem_;
  std::vector<uint32_t> sectorErases_;
  std::vector<Mapping> maps_;
  uint32_t freePages_;
  uint32_t mappedPages_ = 0;
  int64_t budget_ = -1;
  uint64_t steps_ = 0;
  bool dead_ = false;
  bool partialCut_ = false;
  bool failProgram_ = false;
  uint32_t failSkip_ = 0;
  uint32_t violations_ = 0;
  uint32_t reprograms_ = 0;
  uint32_t programCalls_ = 0;
  uint32_t eraseCalls_ = 0;
};
