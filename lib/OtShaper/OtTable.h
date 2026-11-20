#pragma once

#include <cstdint>

// Bounds-checked big-endian views over OpenType table bytes.
//
// Fonts come from users, so every read is checked: a read past the end yields
// zero and an offset past the end yields an empty view, which the layout code
// treats like an absent (null) subtable. Nothing is copied; tables are read in
// place, typically straight from memory-mapped flash.
namespace ot {

constexpr uint32_t tag(const char (&t)[5]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(t[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(t[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(t[2])) << 8) | static_cast<uint8_t>(t[3]);
}

class Table {
 public:
  Table() = default;
  Table(const uint8_t* data, uint32_t length) : data_(length ? data : nullptr), length_(data ? length : 0) {}

  bool empty() const { return length_ == 0; }
  uint32_t length() const { return length_; }
  const uint8_t* data() const { return data_; }

  bool has(const uint32_t offset, const uint32_t count) const { return offset <= length_ && count <= length_ - offset; }
  uint8_t u8(const uint32_t offset) const { return has(offset, 1) ? data_[offset] : 0; }
  uint16_t u16(const uint32_t offset) const {
    return has(offset, 2) ? static_cast<uint16_t>((data_[offset] << 8) | data_[offset + 1]) : 0;
  }
  int16_t s16(const uint32_t offset) const { return static_cast<int16_t>(u16(offset)); }
  uint32_t u24(const uint32_t offset) const {
    return has(offset, 3) ? (static_cast<uint32_t>(data_[offset]) << 16) | (data_[offset + 1] << 8) | data_[offset + 2]
                          : 0;
  }
  uint32_t u32(const uint32_t offset) const {
    return has(offset, 4)
               ? (static_cast<uint32_t>(data_[offset]) << 24) | (static_cast<uint32_t>(data_[offset + 1]) << 16) |
                     (static_cast<uint32_t>(data_[offset + 2]) << 8) | data_[offset + 3]
               : 0;
  }

  // The u16 count stored at `field`, clamped to the `recordSize`-byte records
  // that fit from offset `first` on.
  uint16_t count16(const uint32_t field, const uint32_t first, const uint32_t recordSize) const {
    const uint32_t fits = first <= length_ ? (length_ - first) / recordSize : 0;
    const uint16_t count = u16(field);
    return count < fits ? count : static_cast<uint16_t>(fits);
  }

  // The subtable `offset` bytes in; empty for a null (zero) or out-of-range offset.
  Table at(const uint32_t offset) const {
    if (offset == 0 || offset >= length_) return Table();
    return Table(data_ + offset, length_ - offset);
  }
  // The subtable whose 16-bit (or 32-bit) offset is stored at `field`.
  Table offset16(const uint32_t field) const { return at(u16(field)); }
  Table offset32(const uint32_t field) const { return at(u32(field)); }

 private:
  const uint8_t* data_ = nullptr;
  uint32_t length_ = 0;
};

constexpr int NOT_COVERED = -1;

// Coverage index of `glyph`, or NOT_COVERED.
int coverageIndex(const Table& coverage, uint32_t glyph);

// Class of `glyph` in a ClassDef table; 0 for glyphs it does not list.
uint16_t classOf(const Table& classDef, uint32_t glyph);

}  // namespace ot
