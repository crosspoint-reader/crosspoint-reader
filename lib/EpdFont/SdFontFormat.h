/**
 * .epdfont Binary Font Format Specification
 *
 * This format is designed for on-demand loading from SD card
 * with minimal RAM usage on embedded devices.
 *
 * File Layout:
 * ┌─────────────────────────────────────────────────────┐
 * │ Header (32 bytes)                                   │
 * ├─────────────────────────────────────────────────────┤
 * │ Intervals[] (intervalCount × 12 bytes)              │
 * ├─────────────────────────────────────────────────────┤
 * │ Glyphs[] (glyphCount × 16 bytes)                    │
 * ├─────────────────────────────────────────────────────┤
 * │ Bitmap data (variable size)                         │
 * └─────────────────────────────────────────────────────┘
 */

#pragma once
#include <cstdint>

// Magic number: "EPDF" in little-endian
#define EPDFONT_MAGIC 0x46445045

// Current format version
#define EPDFONT_VERSION 1

#pragma pack(push, 1)

/**
 * File header - 32 bytes
 */
struct EpdFontHeader {
  uint32_t magic;            // 0x46445045 ("EPDF")
  uint16_t version;          // Format version (1)
  uint8_t is2Bit;            // 1 = 2-bit grayscale, 0 = 1-bit
  uint8_t reserved1;         // Reserved for alignment
  uint8_t advanceY;          // Line height
  int8_t ascender;           // Max height above baseline
  int8_t descender;          // Max depth below baseline (negative)
  uint8_t reserved2;         // Reserved for alignment
  uint32_t intervalCount;    // Number of unicode intervals
  uint32_t glyphCount;       // Total number of glyphs
  uint32_t intervalsOffset;  // File offset to intervals array
  uint32_t glyphsOffset;     // File offset to glyphs array
  uint32_t bitmapOffset;     // File offset to bitmap data
};

/**
 * Unicode interval - 12 bytes
 * Same as EpdUnicodeInterval but with explicit packing
 */
struct EpdFontInterval {
  uint32_t first;   // First unicode code point
  uint32_t last;    // Last unicode code point
  uint32_t offset;  // Index into glyph array
};

/**
 * Glyph data - 16 bytes
 * Same as EpdGlyph but with explicit packing
 */
struct EpdFontGlyph {
  uint8_t width;        // Bitmap width in pixels
  uint8_t height;       // Bitmap height in pixels
  uint8_t advanceX;     // Horizontal advance
  uint8_t reserved;     // Reserved for alignment
  int16_t left;         // X offset from cursor
  int16_t top;          // Y offset from cursor
  uint32_t dataLength;  // Bitmap data size in bytes
  uint32_t dataOffset;  // Offset into bitmap section
};

#pragma pack(pop)

// Sanity checks for struct sizes
static_assert(sizeof(EpdFontHeader) == 32, "EpdFontHeader must be 32 bytes");
static_assert(sizeof(EpdFontInterval) == 12, "EpdFontInterval must be 12 bytes");
static_assert(sizeof(EpdFontGlyph) == 16, "EpdFontGlyph must be 16 bytes");
