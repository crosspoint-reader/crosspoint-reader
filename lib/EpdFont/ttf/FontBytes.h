#pragma once

#include <cstdint>
#include <cstring>

namespace ttf {

/// A font file read in pieces (on the SD card), not mapped.
struct FileSource {
  uint32_t size = 0;
  /// Copies `len` bytes at `offset` to `dst`. False on an I/O error.
  bool (*read)(void* ctx, uint32_t offset, void* dst, uint32_t len) = nullptr;
  void* ctx = nullptr;
  /// What the card says of the file beyond its size: FNV-1a over its path and modification time (0: not known). An
  /// SD-mode style keeps it from the install, so a store whose file was replaced is installed again.
  uint32_t stamp = 0;
};

class SdPageCache;

/// SD mode (spec 5.1): the part of a font read from its file on the card instead of the mapped tables, the glyph
/// outlines. `fontOffset` is where the engine addresses it, past the mapped tables; `fileOffset` where it lies in
/// the file.
struct SdRange {
  FileSource file;
  uint32_t fontOffset = 0;
  uint32_t fileOffset = 0;
  uint32_t length = 0;
  SdPageCache* cache = nullptr;  ///< null: every read of the range fails as an I/O error
};

/// The SD card reads of every SD-mode style of a set (spec 5.1): eight 512-byte pages of their files, the least
/// recently used one read over. Pages start on 512-byte file offsets, so each is one sector the card reads straight
/// into the page. Outline reads should move forward through a file: a backward seek makes SdFat walk the file's
/// cluster chain from its start (4.5 ms per read in a 19.7 MB font, 0.35 ms in file order), and the pages only
/// spare the reads that fall into one of them again.
class SdPageCache {
 public:
  static constexpr uint32_t kPageBytes = 512;
  static constexpr uint8_t kPages = 8;

  /// Copies `len` bytes at `offset` of `file` to `dst`. False on a read error, or a range past the file. Pages are
  /// told apart by the file's ctx, so one without a ctx is not read.
  bool read(const FileSource& file, uint32_t offset, uint32_t len, void* dst);
  /// Pages read from the card since the cache was made.
  uint32_t misses() const { return misses_; }

 private:
  const uint8_t* page(const FileSource& file, uint32_t index);

  uint8_t data_[kPages][kPageBytes] = {};
  const void* owner_[kPages] = {};  // the file's ctx; null: an empty page
  uint32_t index_[kPages] = {};     // the page's file offset / kPageBytes
  uint32_t used_[kPages] = {};      // the clock at the last hit: the smallest goes first
  uint32_t clock_ = 0;
  uint32_t misses_ = 0;
  uint8_t last_ = 0;
};

}  // namespace ttf

/// The TTF engine's only way to read a font file (spec 5.1). Values are big-endian, as in every TrueType
/// table, and every access is bounds-checked against the file: a read past its end returns zeros and sets
/// outOfRange(), which means the font is broken (permanent). ioError() is the transient kind, an SD read
/// that failed (SD mode only).
///
/// Flash mode: the whole file is addressable at `data` (the mapping of an installed style, or a buffer in the
/// host tests). SD mode: the style's mapped tables are at `data`, and its outlines (an SdRange) past them are read
/// from the card. FontBytes owns neither, and the caller re-points it when the mapping moves.
class FontBytes {
 public:
  FontBytes() = default;
  FontBytes(const uint8_t* data, const uint32_t size) : data_(data), inPlace_(size), size_(size) {}
  /// SD mode: `size` bytes of tables at `data`, then `sd`, which must outlive the FontBytes.
  FontBytes(const uint8_t* data, const uint32_t size, const ttf::SdRange* sd)
      : data_(data), inPlace_(size), size_(sd->fontOffset + sd->length), sd_(sd) {}

  uint32_t size() const { return size_; }
  /// `len` bytes at `offset` in place, for readers that take a pointer and bound themselves (the SDK's OpenType
  /// layout readers); nullptr unless all of them are. In SD mode that is every table but the outlines.
  const uint8_t* span(const uint32_t offset, const uint32_t len) const {
    return offset <= inPlace_ && len <= inPlace_ - offset ? data_ + offset : nullptr;
  }

  uint8_t u8(const uint32_t offset) {
    const uint8_t* p = at(offset, 1);
    return p ? p[0] : 0;
  }
  uint16_t u16(const uint32_t offset) {
    const uint8_t* p = at(offset, 2);
    return p ? static_cast<uint16_t>(p[0] << 8 | p[1]) : 0;
  }
  int16_t i16(const uint32_t offset) { return static_cast<int16_t>(u16(offset)); }
  uint32_t u32(const uint32_t offset) {
    const uint8_t* p = at(offset, 4);
    if (!p) return 0;
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  }
  /// Copies `len` bytes at `offset` to `dst`. False unless all of them are in the file and could be read.
  bool read(const uint32_t offset, const uint32_t len, void* dst) {
    if (const uint8_t* p = span(offset, len)) {
      if (len > 0) std::memcpy(dst, p, len);
      return true;
    }
    return readFar(offset, len, dst);
  }

  bool outOfRange() const { return outOfRange_; }
  bool ioError() const { return ioError_; }
  /// Forgets both flags. The engine clears them before each glyph, so a broken glyph does not taint the next.
  void clearErrors() {
    outOfRange_ = false;
    ioError_ = false;
  }

 private:
  // Up to four bytes at `offset`: in place, or read from the card into far_; nullptr with a flag set.
  const uint8_t* at(const uint32_t offset, const uint32_t len) {
    if (const uint8_t* p = span(offset, len)) return p;
    return readFar(offset, len, far_) ? far_ : nullptr;
  }
  // Out of line (FontBytes.cpp), so the many inlined reads pay one call for what is not in place: the SD range,
  // through its cache; anything else is out of range.
  bool readFar(uint32_t offset, uint32_t len, void* dst);

  const uint8_t* data_ = nullptr;
  uint32_t inPlace_ = 0;
  uint32_t size_ = 0;
  const ttf::SdRange* sd_ = nullptr;
  uint8_t far_[4] = {};
  bool outOfRange_ = false;
  bool ioError_ = false;
};
