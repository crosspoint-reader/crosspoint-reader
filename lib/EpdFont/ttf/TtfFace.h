#pragma once

#include <cstdint>

#include "FontBytes.h"

/// One TrueType font file as the TTF engine reads it: where its tables are, its metrics, and codepoint to
/// glyph lookup. A Face holds offsets, never pointers, so it stays valid when the file's mapping moves; the
/// FontBytes is passed to every call.
///
/// Sizes and metrics reproduce FreeType 2.13 as the PC converter (fontconvert_sdcard.py) drives it:
/// FT_Set_Char_Size at 150 dpi, advances from linearHoriAdvance, line metrics from the size metrics. A TTF
/// at 14 pt therefore lays out like a .cpfont converted at 14 pt (spec 5.2). Integer only.
namespace ttf {

enum class FaceStatus : uint8_t {
  Ok,
  Unsupported,  ///< a CFF font ('OTTO'), a collection ('ttcf'), or no Unicode cmap of format 4 or 12
  Malformed,    ///< not a font, or a required table is missing, outside the file or impossible
  Transient,    ///< an SD read failed (Stage 2c): try again later
};

struct Face {
  uint32_t glyf = 0;
  uint32_t glyfLength = 0;
  uint32_t loca = 0;
  uint32_t locaLength = 0;
  uint32_t hmtx = 0;
  uint32_t hmtxLength = 0;
  uint32_t cmapSubtable = 0;  ///< the subtable glyphIndex() reads
  uint32_t cmapEnd = 0;       ///< end of the cmap table: the subtable's reads stay below it
  uint16_t unitsPerEm = 0;
  uint16_t numGlyphs = 0;
  uint16_t numHMetrics = 0;
  uint16_t maxPoints = 0;    ///< the larger of maxp's maxPoints and maxCompositePoints
  uint16_t maxContours = 0;  ///< the larger of maxp's maxContours and maxCompositeContours
  int16_t xMin = 0;          ///< head: the box around every glyph, in font units
  int16_t yMin = 0;
  int16_t xMax = 0;
  int16_t yMax = 0;
  int16_t ascender = 0;  ///< FreeType's face metrics: hhea, or OS/2's when it asks (USE_TYPO_METRICS)
  int16_t descender = 0;
  int16_t height = 0;  ///< ascender - descender + line gap
  uint8_t cmapFormat = 0;
  bool longLoca = false;
};

/// Reads the table directory and the tables every glyph needs. Read errors set the FontBytes flags as well.
FaceStatus parseFace(FontBytes& bytes, Face* face);

/// The glyph for `codepoint`, or 0 (the font has none, or the cmap is broken: see bytes.outOfRange()).
uint16_t glyphIndex(FontBytes& bytes, const Face& face, uint32_t codepoint);

/// 26.6 pixels per em for a point size at the converter's 150 dpi, as FreeType derives it: 14 pt -> 1867.
constexpr uint32_t ppemFromPoints(const uint32_t points) { return (points * 64 * 150 + 36) / 72; }

/// The 16.16 factor from font units to 26.6 pixels at `ppem26_6` (FreeType's x_scale, equal to y_scale).
int32_t unitsToPixels(const Face& face, uint32_t ppem26_6);

/// hmtx for `glyph`, read as FreeType reads it: glyphs past numHMetrics share the last advance, and an entry
/// past the end of the table reads 0.
void horizontalMetrics(FontBytes& bytes, const Face& face, uint16_t glyph, uint16_t* advance, int16_t* lsb);

/// The glyph's advance in 12.4 pixels, as the converter derives advanceX from FreeType's linearHoriAdvance.
/// Unclipped: a value over 0xFFFF does not fit EpdGlyph::advanceX, and the caller refuses the glyph.
uint32_t advance12_4(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6);

/// Line metrics in whole pixels, as fontconvert_sdcard.py writes them: advanceY from the rounded height,
/// the ascender rounded up, the descender rounded down.
struct LineMetrics {
  int32_t advanceY = 0;
  int32_t ascender = 0;
  int32_t descender = 0;
};
LineMetrics lineMetrics(const Face& face, uint32_t ppem26_6);

}  // namespace ttf
