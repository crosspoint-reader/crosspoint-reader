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

/// Placement of a mark relative to its base glyph.  The default heuristic —
/// centered over the base, raised clear of its top — suits Latin diacritics
/// and Arabic harakat, but misplaces the Hebrew niqqud whose identity depends
/// on position: dagesh sits inside the letter body, the shin/sin dots
/// distinguish the letter by sitting over its right/left arm, and holam hangs
/// over the left corner.  "Native" anchors keep the glyph's font-designed
/// height (which may overlap the base) instead of raising it.
enum class Anchor : uint8_t {
  CenterRaised,  ///< centered over the base, lifted above its top (default)
  CenterNative,  ///< centered over the base at font-native height
  RightNative,   ///< right edges aligned, font-native height
  LeftNative,    ///< left edges aligned, font-native height
};

constexpr Anchor anchorFor(const uint32_t cp) {
  switch (cp) {
    case 0x05BC:  // dagesh / mapiq / shuruk dot: inside the letter body
    case 0x05BA:  // holam haser for vav: straight above the vav stem
      return Anchor::CenterNative;
    case 0x05C1:  // shin dot: over the letter's right arm
      return Anchor::RightNative;
    case 0x05B9:  // holam: above the letter's left corner
    case 0x05C2:  // sin dot: over the letter's left arm
      return Anchor::LeftNative;
    default:
      return Anchor::CenterRaised;
  }
}

/// Horizontal offset from the base bitmap's left edge to the mark bitmap's
/// left edge for a given anchor.
constexpr int anchorShift(const Anchor anchor, const int baseWidth, const int markWidth) {
  switch (anchor) {
    case Anchor::LeftNative:
      return 0;
    case Anchor::RightNative:
      return baseWidth - markWidth;
    default:
      return baseWidth / 2 - markWidth / 2;
  }
}

/// Compute the cursor-X at which to render a combining mark so its bitmap
/// lands at its anchor position over the base glyph's bitmap.
constexpr int anchorOver(const Anchor anchor, const int baseCursorPos, const int baseLeft, const int baseWidth,
                         const int markLeft, const int markWidth) {
  return baseCursorPos + baseLeft + anchorShift(anchor, baseWidth, markWidth) - markLeft;
}

/// Rotated-90CW variant of anchorOver.  In the rotated coordinate system
/// renderCharImpl uses (cursorY - left) instead of (cursorX + left), so
/// every left/width term inverts sign.
constexpr int anchorOverRotated90CW(const Anchor anchor, const int baseCursorPos, const int baseLeft,
                                    const int baseWidth, const int markLeft, const int markWidth) {
  return baseCursorPos - baseLeft - anchorShift(anchor, baseWidth, markWidth) + markLeft;
}

/// For combining marks that sit entirely above the baseline, compute how many
/// pixels to raise the mark so there is at least MIN_GAP_PX between its bottom
/// edge and the top of the base glyph.  Returns 0 for marks that extend to or
/// below the baseline (e.g. cedilla, dot-below, ogonek) and for anchors that
/// keep the font-native height (dagesh must stay inside the letter, the
/// shin/sin dots touch its arms).
constexpr int raiseAboveBase(const Anchor anchor, const int markTop, const int markHeight, const int baseTop) {
  if (anchor != Anchor::CenterRaised) return 0;
  if (markTop - markHeight <= 0) return 0;
  const int gap = markTop - markHeight - baseTop;
  return (gap < MIN_GAP_PX) ? (MIN_GAP_PX - gap) : 0;
}

}  // namespace combiningMark

/// GCC/Clang (the ESP32 firmware toolchain) pack structs with __attribute__((packed)).
/// MSVC (host unit tests) has no equivalent attribute and instead needs a #pragma pack
/// region achieving the same 1-byte alignment. These macros keep the on-disk font layout
/// identical across both toolchains.
#if defined(_MSC_VER)
#define EPD_PACKED_BEGIN __pragma(pack(push, 1))
#define EPD_PACKED_END __pragma(pack(pop))
#define EPD_PACKED_ATTR
#else
#define EPD_PACKED_BEGIN
#define EPD_PACKED_END
#define EPD_PACKED_ATTR __attribute__((packed))
#endif

/// Fixed-point conventions used by EpdGlyph and EpdFontData:
///   advanceX:   12.4 unsigned fixed-point in uint16_t  (use fp4::toPixel)
///   kernMatrix:  4.4 signed fixed-point in int8_t      (use fp4::toPixel)
/// Both share 4 fractional bits so they combine directly in an accumulator.

/// Font data stored PER GLYPH
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint16_t advanceX;    ///< Distance to advance cursor (x axis), 12.4 fixed-point in pixels
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap (or within-group offset for compressed fonts)
} EpdGlyph;

/// Compressed font group: a DEFLATE-compressed block of glyph bitmaps
typedef struct {
  uint32_t compressedOffset;  ///< Byte offset into compressed data array
  uint32_t compressedSize;    ///< Compressed DEFLATE stream size
  uint32_t uncompressedSize;  ///< Decompressed size
  uint16_t glyphCount;        ///< Number of glyphs in this group
  uint32_t firstGlyphIndex;   ///< First glyph index in the global glyph array
} EpdFontGroup;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Maps a codepoint to a kerning class ID, sorted by codepoint for binary search.
/// Class IDs are 1-based; codepoints not in the table have implicit class 0 (no kerning).
EPD_PACKED_BEGIN
typedef struct {
  uint16_t codepoint;  ///< Unicode codepoint
  uint8_t classId;     ///< 1-based kerning class ID
} EPD_PACKED_ATTR EpdKernClassEntry;
EPD_PACKED_END

/// Ligature substitution for a specific glyph pair, sorted by `pair` for binary search.
/// `pair` encodes (leftCodepoint << 16 | rightCodepoint) for single-key lookup.
EPD_PACKED_BEGIN
typedef struct {
  uint32_t pair;        ///< Packed codepoint pair (left << 16 | right)
  uint32_t ligatureCp;  ///< Codepoint of the replacement ligature glyph
} EPD_PACKED_ATTR EpdLigaturePair;
EPD_PACKED_END

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
  const EpdFontGroup* groups;                 ///< NULL for uncompressed fonts
  uint16_t groupCount;                        ///< 0 for uncompressed fonts
  const uint16_t* glyphToGroup;               ///< Per-glyph group ID (nullptr for contiguous-group fonts)
  const EpdKernClassEntry* kernLeftClasses;   ///< Sorted left-side class map (nullptr if none)
  const EpdKernClassEntry* kernRightClasses;  ///< Sorted right-side class map (nullptr if none)
  const int8_t* kernMatrix;              ///< Flat leftClassCount x rightClassCount matrix, 4.4 fixed-point in pixels
  uint16_t kernLeftEntryCount;           ///< Entries in kernLeftClasses
  uint16_t kernRightEntryCount;          ///< Entries in kernRightClasses
  uint8_t kernLeftClassCount;            ///< Number of distinct left classes (matrix rows)
  uint8_t kernRightClassCount;           ///< Number of distinct right classes (matrix cols)
  const EpdLigaturePair* ligaturePairs;  ///< Sorted ligature pair table (nullptr if none)
  uint32_t ligaturePairCount;            ///< Number of entries in ligaturePairs

  /// On-demand glyph loading for fonts that don't keep all glyphs in RAM (e.g. SD card fonts).
  /// Called by getGlyph() when a codepoint is not found in the interval table.
  /// Returns a valid EpdGlyph* with correct metadata, or nullptr to fall back to the
  /// replacement glyph.  The returned pointer is valid until the next glyphMissHandler
  /// call that causes a ring-buffer eviction — callers must consume it (measure or draw)
  /// before requesting another missed glyph.
  const EpdGlyph* (*glyphMissHandler)(void* ctx, uint32_t codepoint);

  /// Context pointer for glyphMissHandler (typically SdCardFont*).  Also used by
  /// GfxRenderer::getGlyphBitmap() to retrieve overflow bitmaps via SdCardFont.
  void* glyphMissCtx;

  /// Full-coverage query for fonts whose interval table only reflects what is
  /// currently in RAM (SD card fonts: stub/mini data cover at most one page of
  /// glyphs).  Called by hasCodepoint() when the interval table misses; must
  /// answer from RAM-resident data without storage I/O.  Shares glyphMissCtx.
  /// nullptr for fonts whose interval table is already complete (built-ins).
  bool (*coverageHandler)(void* ctx, uint32_t codepoint);
} EpdFontData;
