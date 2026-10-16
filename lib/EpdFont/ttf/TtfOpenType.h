#pragma once

#include <EpdFontData.h>

#include <cstdint>
#include <memory>

#include "FontBytes.h"
#include "TtfFace.h"

/// Kerning and ligatures for the TTF engine, from the font's layout tables (spec 5.4). The tables are read by the SDK's
/// FreeInkFont readers (Gpos.h, Gsub.h, KernTable.h), shared with its FreeType font; this is the glue: resolving them
/// once per style, and the ligatures as EpdFont applies them. Offsets only, so a resolved style stays valid when the
/// font's mapping moves. Integer only.
namespace ttf {

/// A style's kerning: the GPOS 'kern' feature's subtables, resolved once, or, for a font without them, the legacy
/// 'kern' table. A pair's kerning is what OpenType layout gives (see Gpos.h).
class Kerning {
 public:
  /// More would be a malformed or hostile table: the first ones are kept. More than three times the most found in a
  /// real font (76, Calibri Light), and it bounds what a hostile table costs per pair: the resolved walk has no
  /// budget of its own, so every subtable is read for every pair that misses the store.
  static constexpr uint16_t kMaxSubtables = 256;

  /// Resolves the subtables (one heap block, 4 bytes each). False, with nothing held, for a font without kerning or
  /// when memory is short; kerning is then 0.
  bool resolve(FontBytes& bytes, const Face& face);
  bool any() const { return count_ != 0 || legacy_; }
  uint16_t subtableCount() const { return count_; }
  /// Kerned from the legacy kern table (the font has no GPOS kerning).
  bool legacy() const { return legacy_; }

  /// The horizontal kerning between two glyphs, in font units. `covered` (optional) is false when nothing the font
  /// kerns starts with `left`: such a pair is always 0 and need not be remembered.
  int32_t pair(FontBytes& bytes, const Face& face, uint16_t left, uint16_t right, bool* covered = nullptr) const;

 private:
  std::unique_ptr<uint32_t[]> subtables_;  // offsets into the GPOS table (Gpos.h)
  uint16_t count_ = 0;
  bool legacy_ = false;
};

/// The ligatures EpdFont applies (fi, fl, ff, ffi, ffl) where the font's GSUB has them. They are keyed by their
/// Unicode presentation forms, U+FB00-FB04, whatever the font's cmap says of those: `glyphs` gives the glyph each one
/// draws. ffi and ffl chain through ff, as EpdFont applies them two at a time.
struct Ligatures {
  static constexpr uint32_t kFirst = 0xFB00;  // ff, fi, fl, ffi, ffl
  EpdLigaturePair pairs[5] = {};
  uint8_t count = 0;
  uint16_t glyphs[5] = {};  // for U+FB00 + i; 0 when the font has no such ligature

  /// The glyph that draws `codepoint`: a ligature's, else the cmap's.
  uint16_t glyphFor(FontBytes& bytes, const Face& face, uint32_t codepoint) const;
};

/// Resolves the ligatures; false (none) for a font without them.
bool resolveLigatures(FontBytes& bytes, const Face& face, Ligatures* out);

}  // namespace ttf
