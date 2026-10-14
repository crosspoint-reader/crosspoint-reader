#pragma once

#include <cstdint>
#include <cstring>

/// The TTF engine's only way to read a font file (spec 5.1). Values are big-endian, as in every TrueType
/// table, and every access is bounds-checked against the file: a read past its end returns zeros and sets
/// outOfRange(), which means the font is broken (permanent). ioError() is the transient kind, an SD read
/// that failed; it belongs to the SD mode (Stage 2c) and stays false in flash mode.
///
/// Flash mode: the whole file is addressable at `data` (the mapping of an installed style, or a buffer in the
/// host tests). FontBytes does not own it, and the caller re-points it when the mapping moves.
class FontBytes {
 public:
  FontBytes() = default;
  FontBytes(const uint8_t* data, const uint32_t size) : data_(data), size_(size) {}

  uint32_t size() const { return size_; }

  uint8_t u8(const uint32_t offset) {
    if (!contains(offset, 1)) return fail();
    return data_[offset];
  }
  uint16_t u16(const uint32_t offset) {
    if (!contains(offset, 2)) return fail();
    return static_cast<uint16_t>(data_[offset] << 8 | data_[offset + 1]);
  }
  int16_t i16(const uint32_t offset) { return static_cast<int16_t>(u16(offset)); }
  uint32_t u32(const uint32_t offset) {
    if (!contains(offset, 4)) return fail();
    const uint8_t* p = data_ + offset;
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  }
  /// Copies `len` bytes at `offset` to `dst`. False, with nothing copied, unless all of them are in the file.
  bool read(const uint32_t offset, const uint32_t len, void* dst) {
    if (!contains(offset, len)) {
      fail();
      return false;
    }
    if (len > 0) std::memcpy(dst, data_ + offset, len);
    return true;
  }

  bool outOfRange() const { return outOfRange_; }
  bool ioError() const { return ioError_; }
  /// Forgets both flags. The engine clears them before each glyph, so a broken glyph does not taint the next.
  void clearErrors() {
    outOfRange_ = false;
    ioError_ = false;
  }

 private:
  bool contains(const uint32_t offset, const uint32_t len) const { return offset <= size_ && len <= size_ - offset; }
  uint8_t fail() {
    outOfRange_ = true;
    return 0;
  }

  const uint8_t* data_ = nullptr;
  uint32_t size_ = 0;
  bool outOfRange_ = false;
  bool ioError_ = false;
};
