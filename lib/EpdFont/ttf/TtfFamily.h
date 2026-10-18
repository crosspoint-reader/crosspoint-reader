#pragma once

#include <cstddef>
#include <cstdint>

#include "GlyphStoreFormat.h"

/// What the registry and the manager need to know about a TrueType family before loading it: which style a
/// file is, and the identity that tells an installed glyph store whether it holds this family.
namespace ttf {

/// A loaded family's native sizes have font IDs `base | px`: a window of 256 IDs, its base a multiple of 256 with
/// the top bit clear, so no negative ID is ever in it. FontSizeLadder makes the same IDs (its setNative()).
constexpr uint32_t kNativeIdMask = 0xFFFFFF00u;
inline bool isNativeId(const int32_t base, const int fontId) {
  return base != 0 && (static_cast<uint32_t>(fontId) & kNativeIdMask) == static_cast<uint32_t>(base);
}
inline uint8_t nativeIdPixels(const int fontId) { return static_cast<uint8_t>(fontId & 0xFF); }
/// A window base from `hash`: its bits 8-30. 0 (no window) when they are all clear.
inline int32_t nativeBaseFrom(const uint32_t hash) { return static_cast<int32_t>(hash & 0x7FFFFF00u); }

/// A font file read in pieces (on the SD card), not mapped.
struct FileSource {
  uint32_t size = 0;
  /// Copies `len` bytes at `offset` to `dst`. False on an I/O error.
  bool (*read)(void* ctx, uint32_t offset, void* dst, uint32_t len) = nullptr;
  void* ctx = nullptr;
};

/// A FileSource over an open file with seekSet() and read() (HalFile on the device), `size` bytes long. The file
/// must outlive the source.
template <typename File>
FileSource fileSource(File& file, const uint32_t size) {
  return {size,
          [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
            auto* f = static_cast<File*>(ctx);
            return f->seekSet(offset) && f->read(dst, len) == static_cast<int>(len);
          },
          &file};
}

/// True for a name ending in ".ttf", in any case. In-progress copies ("x.ttf.tmp") do not count.
bool isTtfFileName(const char* name);

/// What discovery learns from a TrueType file, in a few small reads.
struct FileProbe {
  /// head.macStyle: bit 0 bold, bit 1 italic, as EpdFontFamily::Style. -1 when it is not a font the engine
  /// draws: not TrueType (CFF, a collection), a table it needs missing or outside the file, or a read error.
  int style = -1;
  /// OS/2 usWeightClass (400 regular, 700 bold). Without an OS/2 table, 700 for a bold style and 400 otherwise.
  uint16_t weight = 400;
  /// It has an fvar table: a variable font, drawn as its default instance only (spec 5.6).
  bool variable = false;
};
FileProbe probeFile(const FileSource& file);
/// probeFile(file).style.
int probeStyle(const FileSource& file);

/// Which file serves each style of a directory's TrueType files, the files sorted by name. Each style takes, of
/// the files declaring it, a static one before a variable one, then the weight nearest 400 (regular, italic) or
/// 700 (bold, bold italic), then the first by name: a folder of static weights (Light, Regular, ExtraBold, ...,
/// most of them declaring the regular style) then serves Regular, not whichever sorts first. A file of weight 600 or
/// more declaring regular (italic) may serve bold (bold italic) too, unless it serves its own style. `out[style]` is
/// that file's index, or -1. False when no file is regular: not a family the reader can use, since every missing
/// style falls back to regular.
bool assignStyles(const FileProbe* files, size_t count, int (&out)[4]);

/// True when flash mode can install a family with these style file sizes: every style copied into a font area of
/// `usableBytes`, with the largest style, the index and a minimal log mapped within `freeMapPages`
/// (glyphstore::planLayout). Larger families need SD mode (Stage 2c-4).
bool fitsFlashMode(const uint32_t* styleBytes, uint8_t count, uint32_t usableBytes, uint32_t freeMapPages);

/// Bump whenever the engine's output changes for the same font (the rasterizer, the 150 dpi sizing, the 2-bit
/// thresholds): the identity changes with it, so every installed store is rebuilt.
/// 2: kerning and GSUB ligatures (2c-3): U+FB00-FB04 now draw the glyphs GSUB names, so stores holding the cmap's
/// glyphs or a no-glyph marker for them must go.
constexpr uint8_t kRenderVersion = 2;

/// One style's file as the card lists it.
struct FileStamp {
  const char* name = nullptr;  ///< nullptr: the family has no file for this style
  uint32_t size = 0;
  uint16_t date = 0;  ///< FAT modification date and time
  uint16_t time = 0;
};

/// The family identity (spec 4.2): FNV-1a over each style's file name, size and modification time, and
/// kRenderVersion, in four lanes with different seeds. Any change to a file gives a new identity.
void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]);

/// The identity folded to 32 bits, to seed the family's font IDs.
inline uint32_t identityHash(const uint8_t identity[glyphstore::kIdentityBytes]) {
  return glyphstore::fnv1a(identity, glyphstore::kIdentityBytes);
}

}  // namespace ttf
