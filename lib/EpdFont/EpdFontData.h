// From
// https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h

#pragma once
#include <cstdint>

/// Font metrics use "fixed-point 4" (4 fractional bits, i.e. 1/16-pixel
/// resolution).  Both the 12.4 glyph advances (uint16_t) and the 4.4 kern
/// values (int8_t) share the same 4 fractional bits, so they can be freely
/// added before snapping to whole pixels.
///
/// Rendering and measurement use "differential rounding": each glyph step
/// (previous advance + current kern) is combined in fixed-point and snapped
/// to a pixel as one unit.  This guarantees identical character pairs always
/// produce the same pixel spacing, regardless of position on the line.
///
/// The helpers below eliminate the raw bit-shifts that would otherwise be
/// scattered across every layout / measurement call site.
namespace fp4 {
constexpr int FRAC_BITS = 4;
constexpr int32_t HALF = 1 << (FRAC_BITS - 1);  // 8, added before shift for round-to-nearest

/// Convert an integer pixel value to 12.4 fixed-point.
constexpr int32_t fromPixel(int px) { return static_cast<int32_t>(px) << FRAC_BITS; }

/// Snap a fixed-point value to the nearest integer pixel.
constexpr int toPixel(int32_t fp) { return static_cast<int>((fp + HALF) >> FRAC_BITS); }

/// Convert a fixed-point value to float (mainly useful for debug logging).
constexpr float toFloat(int32_t fp) { return fp / static_cast<float>(1 << FRAC_BITS); }
}  // namespace fp4

/// Helpers for positioning Unicode combining marks (U+0300 ff.) over a
/// preceding base glyph without GPOS anchor tables.
namespace combiningMark {

constexpr int MIN_GAP_PX = 1;

/// Compute the cursor-X at which to render a combining mark so its bitmap
/// is visually centered over the base glyph's bitmap.
constexpr int centerOver(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos + baseLeft + baseWidth / 2 - markWidth / 2 - markLeft;
}

/// Rotated-90CW variant of centerOver.  In the rotated coordinate system
/// renderCharImpl uses (cursorY - left) instead of (cursorX + left), so
/// every left/width term inverts sign.
constexpr int centerOverRotated90CW(int baseCursorPos, int baseLeft, int baseWidth, int markLeft, int markWidth) {
  return baseCursorPos - baseLeft - baseWidth / 2 + markWidth / 2 + markLeft;
}

/// For combining marks that sit entirely above the baseline, compute how many
/// pixels to raise the mark so there is at least MIN_GAP_PX between its bottom
/// edge and the top of the base glyph.  Returns 0 for marks that extend to or
/// below the baseline (e.g. cedilla, dot-below, ogonek).
constexpr int raiseAboveBase(int markTop, int markHeight, int baseTop) {
  if (markTop - markHeight <= 0) return 0;
  const int gap = markTop - markHeight - baseTop;
  return (gap < MIN_GAP_PX) ? (MIN_GAP_PX - gap) : 0;
}

}  // namespace combiningMark

/// Fixed-point conventions used by EpdGlyph and EpdFontData:
///   advanceX:   12.4 unsigned fixed-point in uint16_t  (use fp4::toPixel)
///   kernMatrix:  4.4 signed fixed-point in int8_t      (use fp4::toPixel)
/// Both share 4 fractional bits so they combine directly in an accumulator.

/// Font data stored PER GLYPH — and the .cpfont FILE FORMAT.
///
/// SD-card fonts memory-map this array straight out of the file (SdCardFont.cpp asserts the
/// size), so the layout is frozen: changing it would invalidate every .cpfont already on a
/// user's card. Built-in fonts use EpdGlyphPacked below, which is half the size.
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint16_t advanceX;    ///< Distance to advance cursor (x axis), 12.4 fixed-point in pixels
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap (or within-group offset for compressed fonts)
} EpdGlyph;

/// The same information for a BUILT-IN font, in half the bytes.
///
/// The glyph table is the largest uncompressed structure in the image — 56,129 glyphs at 16 bytes
/// would be 898,064 B — and ten of those bytes are recoverable. Measured over every shipped
/// glyph, and re-checked by fontconvert.py at generation time:
///
///   * `dataLength` is exactly the packed bitmap size, so it is derived rather than stored (see
///     glyphDataBytes()). Bitmaps are a continuous bit stream with no row stride.
///   * `left` spans only -22..11 and `top` -4..42, so both fit int8_t.
///   * `dataOffset` is the running sum of those derived lengths, so it is not stored either.
///     Verified across all 56,129 shipped glyphs with zero mismatches. Crucially this costs
///     nothing to reconstruct, because the only paths that ever wanted it were already summing
///     the same prefix: FontDecompressor::getAlignedOffset() walks the preceding glyphs of the
///     group to find where a glyph starts in the decoded stream, and never read the stored
///     field at all. On a compressed font the field was pure dead weight.
///
///     Uncompressed fonts are the exception — they index a flat bitmap array directly, with no
///     walk to piggyback on — so they carry EpdFontData::bitmapOffsets instead. That is the same
///     two bytes per glyph, but only for the 7 faces that need it rather than all 55.
///
/// **Why this does not simply replace EpdGlyph.** Besides the .cpfont format being frozen, SD
/// fonts point straight into the mmap'd file for their glyph table, so a narrower in-memory
/// record would force an ~8 KB heap copy per font on a device that runs at ~39 KB free. The two
/// representations coexist, selected by whichever pointer EpdFontData carries — the same
/// arrangement the kern matrix and the kern class maps already use.
///
/// **The one ceiling this introduces:** the uint16 entries of EpdFontData::bitmapOffsets cap an
/// UNCOMPRESSED font at 64 KB of bitmap data. Compressed fonts have no such table and are bounded
/// far below that by the per-group cap anyway, so it binds only the 1-bit UI faces;
/// inter_ui_14_bold, the largest today, is at 45,193 B — roughly one more size step of headroom.
/// fontconvert.py fails the build with an explicit message rather than truncating, and the fix
/// then is to widen that one table to uint32, which costs only those few faces.
typedef struct {
  uint8_t width;      ///< Bitmap dimensions in pixels
  uint8_t height;     ///< Bitmap dimensions in pixels
  uint16_t advanceX;  ///< 12.4 fixed-point; at offset 2, so naturally aligned
  int8_t left;        ///< X dist from cursor pos to UL corner
  int8_t top;         ///< Y dist from cursor pos to UL corner
} EpdGlyphPacked;
static_assert(sizeof(void*) != 4 || sizeof(EpdGlyphPacked) == 6,
              "EpdGlyphPacked must stay 6 bytes; it is 56,129 glyphs wide");

/// Bytes a w x h glyph bitmap occupies — what EpdGlyph::dataLength stores and what the packed
/// record derives instead.
///
/// Bitmaps are packed as a CONTINUOUS bit stream: the next row starts in the same byte the
/// previous one ended in, so there is no per-row stride term. That is what makes the field
/// redundant; a row-aligned format would not be derivable this cheaply.
constexpr uint16_t glyphDataBytes(const int width, const int height, const bool is2Bit) {
  return static_cast<uint16_t>((width * height * (is2Bit ? 2 : 1) + 7) / 8);
}

/// One glyph's metrics, resolved from whichever representation its font uses.
///
/// Returned BY VALUE. A pointer cannot serve both forms, and the old `const EpdGlyph*` carried a
/// lifetime rule besides — it was valid only until the next GlyphProvider::missGlyph() call that evicted
/// the overflow ring, which every caller had to honour and nothing enforced. Resolving to values
/// removes that hazard rather than extending it to a second representation.
/// Laid out and initialised for the LOOKUP path, which is much hotter than the draw path: a
/// glyph is resolved once per character every time a string is measured, and measurement runs
/// several times per rendered row.
///
/// Two things here are performance, not style, and both were measured (env:bench_font, X3):
///
///   * **No default member initialisers.** With them, every resolve zero-filled the struct and
///     then wrote every field again -- ~24 bytes of redundant stores per lookup. Costed at
///     +635 ns, which took glyph_lookup from 1083 to 1718 ns. Every field is now assigned
///     exactly once, and `valid` is the only thing a default-constructed ref sets.
///   * **Neither dataLength nor dataOffset is here.** Both are derived, and both would be paid
///     for on every resolve by a measurement path that never reads either. The handful of sites
///     that actually want them call glyphDataBytes() and epdGlyphBitmapOffset() themselves.
struct EpdGlyphRef {
  /// The underlying record, for SD-card fonts only. GlyphProvider::ownsBitmap() keys on pointer
  /// identity to tell a provider-loaded glyph from an array one,
  /// so that identity has to survive resolution. Always null for built-in fonts.
  const EpdGlyph* sdRecord;
  uint16_t advanceX;  ///< 12.4 fixed-point
  /// Position in the font's glyph array. The bitmap paths key on it; it used to be recovered by
  /// subtracting the array base from the returned pointer, which a packed array would break.
  /// uint16 because no shipped face has more than ~1,200 glyphs.
  uint16_t index;
  uint8_t width;
  uint8_t height;
  int8_t left;
  int8_t top;
  /// The ONLY field a default-constructed ref sets, so `return {}` stays cheap.
  bool valid = false;

  /// So `if (!glyph)` at the call sites keeps reading the way it did with a pointer.
  explicit operator bool() const { return valid; }
  /// And so `glyph->width` does too. Not for this repo's benefit -- everything here uses `.` --
  /// but for the shared freeink-sdk, whose FreeInkUIGfxRenderer reads glyph metrics through
  /// whatever font API the app it is built against provides. Upstream crosspoint-reader still
  /// returns `const EpdGlyph*`, so the SDK has to compile against both, and `auto` plus `->` is
  /// the spelling that works either way.
  const EpdGlyphRef* operator->() const { return this; }
};

/// Compressed font group: a DEFLATE-compressed block of glyph bitmaps
typedef struct {
  uint32_t compressedOffset;  ///< Byte offset into compressed data array
  uint32_t compressedSize;    ///< Compressed DEFLATE stream size
  uint32_t uncompressedSize;  ///< Decompressed size
  uint16_t glyphCount;        ///< Number of glyphs in this group
  /// Bytes of decoded history this group's DEFLATE stream can reach back into — i.e. the
  /// largest back-reference distance the encoder actually emitted, which fontconvert.py
  /// measures by parsing the finished stream.
  ///
  /// This is what the decoder has to hold in RAM, and it is NOT uncompressedSize: the group
  /// is streamed through a ring of exactly this size and each glyph is compacted straight out
  /// of the stream (FontDecompressor::GroupStream), so the group never exists in memory at
  /// once. Decoupling the two is the whole point — the group may be as large as compression
  /// likes while the transient allocation stays small and, unlike a malloc sized by the data,
  /// predictable.
  ///
  /// 0 means "not measured" and the decoder falls back to uncompressedSize, which is always
  /// safe (a reference can never outrun the bytes produced so far). That keeps fonts generated
  /// before this field was added decodable.
  ///
  /// Sits here rather than at the end on purpose: it fills the padding hole after glyphCount,
  /// so the struct is still 20 bytes and the table costs no extra flash (static_assert in
  /// FontDecompressor.cpp).
  uint16_t ringBytes;
  uint32_t firstGlyphIndex;  ///< First glyph index in the global glyph array
} EpdFontGroup;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
/// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} __attribute__((packed)) EpdKernClassEntry;

/// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
/// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} __attribute__((packed)) EpdLigaturePair;

class GlyphProvider;

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;  ///< Glyph bitmaps, concatenated
  /// Glyph array, 16 bytes an entry. SD-card fonts only — null for built-in fonts, which carry
  /// `glyphPacked` instead. Exactly one of the two is non-null, and EpdFont resolves whichever is
  /// present, the same way getKerning() picks between the dense and sparse matrices.
  const EpdGlyph* glyph;
  /// Glyph array, 8 bytes an entry. Built-in fonts only — null for SD-card fonts. See
  /// EpdGlyphPacked for why the two coexist rather than converging.
  const EpdGlyphPacked* glyphPacked;
  /// Byte offset of each glyph's bitmap within `bitmap`, parallel to the glyph array.
  ///
  /// NON-NULL ONLY FOR UNCOMPRESSED BUILT-IN FONTS, which index `bitmap` directly and so have no
  /// group walk to recover the offset from. Compressed fonts leave it null and SD fonts carry the
  /// offset in their own record; both would only be paying for a table nothing reads.
  ///
  /// Read through epdGlyphBitmapOffset(), never directly.
  const uint16_t* bitmapOffsets;
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  /// Two bits per pixel rather than one. Also drives the packed glyph record's dataLength
  /// derivation (see glyphDataBytes), so a hand-built EpdFontData that leaves this false for a
  /// 2-bit font halves every glyph's length silently.
  bool is2Bit;
  const EpdFontGroup* groups;                 ///< NULL for uncompressed fonts
  uint16_t groupCount;                        ///< 0 for uncompressed fonts
  const uint16_t* glyphToGroup;               ///< Per-glyph group ID (nullptr for contiguous-group fonts)
  const EpdKernClassEntry* kernLeftClasses;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses;  ///< Sorted right-side class map (nullptr if none)
  /// Split form of the two class maps, used by the built-in fonts instead of the packed
  /// EpdKernClassEntry arrays above (which are left null for them). Same total size — 2 + 1 bytes
  /// per entry either way — but measurably faster: the class lookups are ~96% of getKerning(),
  /// and splitting them measured -13 to -14% on that path. Two reasons. The binary search only
  /// ever reads codepoints, so keeping the classId payload out of the searched array shrinks its
  /// footprint by a third; and a uint16 array is naturally aligned where a 3-byte packed struct
  /// leaves two of every three codepoint reads unaligned.
  ///
  /// SD-card fonts keep the packed form: the .cpfont format stores it verbatim and maps it in
  /// place (see the static_asserts in SdCardFont.cpp). Same coexistence rule as the kerning
  /// matrix below — whichever pointer is non-null selects the representation.
  const uint16_t* kernLeftCodepoints;   ///< nullptr when this font uses the packed form
  const uint8_t* kernLeftClassIds;      ///< parallel to kernLeftCodepoints
  const uint16_t* kernRightCodepoints;  ///< nullptr when this font uses the packed form
  const uint8_t* kernRightClassIds;     ///< parallel to kernRightCodepoints
  const int8_t* kernMatrix;             ///< Flat leftClassCount x rightClassCount matrix, 4.4 fixed-point in pixels
  /// Sparse (CSR) kerning — the built-in fonts use this instead of `kernMatrix`, which is left
  /// null for them. Measured across the built-in set, 504682 of 583049 dense entries are zero
  /// (86.6%), so storing only the non-zero ones costs ~165 KB where the dense matrices cost
  /// ~583 KB. Values are identical, so layout and pagination are unaffected.
  ///
  /// SD-card fonts keep using `kernMatrix`: the .cpfont format stores the dense matrix verbatim
  /// and is memory-mapped in place (see the static_asserts in SdCardFont.cpp), so switching them
  /// would break every font file already on a user's card. getKerning() reads whichever is
  /// present, so the two representations coexist with one branch.
  ///
  /// kernRowOffsets has kernLeftClassCount + 1 entries; row `l` occupies
  /// [kernRowOffsets[l], kernRowOffsets[l+1]) in the other two arrays, sorted ascending by
  /// column so the lookup can binary-search it.
  const uint16_t* kernRowOffsets;        ///< nullptr when this font uses the dense matrix
  const uint8_t* kernSparseCols;         ///< 0-based right class of each stored entry
  const int8_t* kernSparseValues;        ///< 4.4 fixed-point value of each stored entry
  uint16_t kernLeftEntryCount;           ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount;          ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount;            ///< Number of distinct left classes (matrix rows)
  uint8_t kernRightClassCount;           ///< Number of distinct right classes (matrix cols)
  const EpdLigaturePair* ligaturePairs;  ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount;            ///< Number of entries in ligaturePairs

  /// Glyphs the tables above do not have, and kerning for a font with no kern tables. Null for
  /// built-in fonts. See GlyphProvider.h.
  GlyphProvider* provider;
} EpdFontData;

/// Reads glyph `index` out of whichever representation `data` carries.
///
/// The single place that knows EpdGlyph and EpdGlyphPacked both exist. Inline because it runs
/// once per glyph in the layout and render loops.
inline EpdGlyphRef epdResolveGlyph(const EpdFontData* data, const uint32_t index) {
  // One aggregate construction per branch, in declaration order. Deliberately NOT a default
  // construction followed by assignments: that wrote every field twice and cost +59% on
  // glyph_lookup. See the note on EpdGlyphRef.
  if (data->glyphPacked) {
    const EpdGlyphPacked& g = data->glyphPacked[index];
    return EpdGlyphRef{nullptr, g.advanceX, static_cast<uint16_t>(index), g.width, g.height, g.left, g.top, true};
  }
  const EpdGlyph& g = data->glyph[index];
  return EpdGlyphRef{&g,  // SD-card fonts only: the overflow-ring checks key on this pointer
                     g.advanceX, static_cast<uint16_t>(index), g.width,
                     g.height,   static_cast<int8_t>(g.left),  static_cast<int8_t>(g.top),
                     true};
}

/// Byte offset of a glyph's bitmap within EpdFontData::bitmap.
///
/// Only meaningful for fonts that HAVE a flat bitmap array — uncompressed built-ins and SD fonts.
/// A compressed built-in has no such array; its bitmap comes out of a decoded group, and
/// FontDecompressor computes the position there by walking the group itself.
///
/// Out of line from the resolve for the reason dataLength is: the measurement path resolves a
/// glyph per character and would otherwise pay for an offset it never reads.
inline uint32_t epdGlyphBitmapOffset(const EpdFontData* data, const EpdGlyphRef& glyph) {
  // SD-card fonts: the .cpfont record is frozen and still stores it. This also covers a glyph
  // loaded into the overflow ring, which has no array index to look up.
  if (glyph.sdRecord) return glyph.sdRecord->dataOffset;
  return data->bitmapOffsets ? data->bitmapOffsets[glyph.index] : 0;
}
