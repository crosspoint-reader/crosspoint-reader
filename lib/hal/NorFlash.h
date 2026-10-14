#pragma once

#include <cstdint>

/// A region of NOR flash. Bits program from 1 to 0 only; only an erase, in whole 4 KB sectors, sets them
/// back to 1. FlashGlyphStore works through this interface, so its write-once rules run unchanged on
/// the device (EspNorFlash, over the `spiffs` partition) and in host tests (a simulated chip).
///
/// Offsets are relative to the start of the region. The device region stops below the language slot at
/// the end of the partition (FlashFontPartition::LANG_RESERVED_BYTES).
///
/// A power cut during program() leaves the bytes before the cut programmed and the rest erased, except for
/// the data in flight (up to one 64-byte ESP-IDF chunk on the device), whose bits may be partly programmed.
/// A cut can therefore only leave a byte with MORE 1 bits than was written, never fewer; the store's
/// torn-write rules rely on that.
class NorFlash {
 public:
  static constexpr uint32_t kSectorBytes = 4096;    ///< erase granularity
  static constexpr uint32_t kMapPageBytes = 65536;  ///< one MMU page; mappings are paid in whole pages

  virtual ~NorFlash() = default;

  /// Usable bytes in the region.
  virtual uint32_t size() const = 0;
  /// Copies `len` bytes at `offset` to `dst`. False on an I/O error or a range outside the region.
  virtual bool read(uint32_t offset, void* dst, uint32_t len) const = 0;
  /// Programs `len` bytes. A target bit can only go from 1 to 0; the caller never asks for 0 -> 1.
  virtual bool program(uint32_t offset, const void* src, uint32_t len) = 0;
  /// Erases whole sectors to 0xFF. `offset` and `len` must be multiples of kSectorBytes.
  virtual bool erase(uint32_t offset, uint32_t len) = 0;
  /// Maps `len` bytes at `offset` for reading in place; nullptr when the mapping budget is exhausted.
  /// Reads through a mapping see every later program() and erase().
  virtual const uint8_t* map(uint32_t offset, uint32_t len) = 0;
  /// Releases a mapping returned by map().
  virtual void unmap(const uint8_t* mapped) = 0;
  /// MMU pages still free for data mappings.
  virtual uint32_t freeMapPages() const = 0;
};
