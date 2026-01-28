#pragma once

// 16-bit marker at the start of a CrossPoint font file
// (CrossPoint Font Format)
#define CPF_FONT_MARKER 0xCFF1
#define CPF_FONT_MARKER_SMALL 0xCFF2

// Font info per large character (glyph)
typedef struct {
  uint32_t bitmapOffset;  /// Offset to compressed bitmap data for this glyph
  uint16_t width;         /// bitmap width in pixels
  uint16_t height;        /// bitmap height in pixels
  uint16_t xAdvance;      /// total width in pixels (bitmap + padding)
  int16_t xOffset;        /// left padding to upper left corner
  int16_t yOffset;        /// top padding to upper left corner
} CrossPointFontGlyph;

// Font info per small character (glyph)
typedef struct {
  uint32_t bitmapOffset;  /// Offset to compressed bitmap data for this glyph
  uint8_t width;          /// bitmap width in pixels
  uint8_t height;         /// bitmap height in pixels
  uint8_t xAdvance;       /// total width in pixels (bitmap + padding)
  int8_t xOffset;         /// left padding to upper left corner
  int16_t yOffset;        /// top padding to upper left corner
} CrossPointFontSmallGlyph;

/// Glyph interval structure
typedef struct {
  uint32_t first;   /// The first unicode code point of the interval
  uint32_t last;    /// The last unicode code point of the interval
  uint32_t offset;  /// Index of the first code point into the glyph array
} CrossPointFontUnicodeInterval;

typedef struct {
  uint16_t u16Marker;      /// CPF_FONT_MARKER / CPF_FONT_MARKER_SMALL
  uint16_t height;         /// Newline distance (y axis)
  uint16_t ascender;       /// Maximal height of a glyph above the base line
  uint8_t styles;          /// Regular = 0x01, Bold = 0x02, Italic = 0x04, BoldItalic = 0x08, can be OR'd together
  uint16_t intervalCount;  /// Number of unicode intervals.
  uint32_t glyphCount;     /// Number of total glyphs across all styles
} CrossPointFontHeader;

/// Data stored for FONT AS A WHOLE
typedef struct {
  CrossPointFontHeader header;
  CrossPointFontUnicodeInterval* intervals;  /// Valid unicode intervals for this font
  CrossPointFontGlyph* glyphs;               /// Glyph array
  uint8_t* bitmap;                           /// Glyph bitmaps, concatenated
} CrossPointFontData;

typedef struct {
  CrossPointFontHeader header;
  CrossPointFontUnicodeInterval* intervals;  /// Valid unicode intervals for this font
  CrossPointFontSmallGlyph* glyphs;          /// Glyph array
  uint8_t* bitmap;                           /// Glyph bitmaps, concatenated
} CrossPointFontDataSmall;
