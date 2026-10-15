#pragma once

#include <cstddef>
#include <cstdint>

#include "GlyphStoreFormat.h"

/// What the registry and the manager need to know about a TrueType family before loading it: which style a
/// file is, and the identity that tells an installed glyph store whether it holds this family.
namespace ttf {

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

/// The style a TrueType file declares in head.macStyle: bit 0 bold, bit 1 italic, as EpdFontFamily::Style.
/// -1 when it is not a font the engine draws: not TrueType (CFF, a collection), a table it needs missing or
/// outside the file, or a read error.
int probeStyle(const FileSource& file);

/// Which file serves each style of a directory's TrueType files. `styles[i]` is probeStyle() of file i, the
/// files sorted by name; the first file of each style wins, so a family does not change with the card's listing
/// order. `out[style]` is that file's index, or -1. False when no file is regular: not a family the reader can
/// use, since every missing style falls back to regular.
bool assignStyles(const int* styles, size_t count, int (&out)[4]);

/// Bump whenever the engine's output changes for the same font (the rasterizer, the 150 dpi sizing, the 2-bit
/// thresholds): the identity changes with it, so every installed store is rebuilt.
constexpr uint8_t kRenderVersion = 1;

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
