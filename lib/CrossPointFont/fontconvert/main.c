//
// TrueType to CrossPoint font converter
// Copyright (c) 2024 BitBank Software, inc.
// Written by Larry Bank, adapted by Dave Allie for CrossPoint
// August 31, 2024
// The CrossPoint font format is a losslessly compressed bitmap font of a single point size with multiple variants
// This was built entirely on the back of Larry Bank's bb_font format.
// The data is compressed with a compression scheme based on CCITT T.6
// The font structure includes overall size, per-character glyph info and then the
// compressed image data at the end.
// The font file format is designed to allow both dynamic loading of font data from
// external memory/disk or compiling the data as const into a progarm.
//
// Example usage:
// ./fontconvert <regular.ttf> [-b <bold.ttf>] [-i <italic.ttf>] [-bi <bold-italic.ttf>] -p <pt size> -o <out.cpf>
// ./fontconvert <regular.ttf> [-b <bold.ttf>] [-i <italic.ttf>] [-bi <bold-italic.ttf>] -p <pt size> -o <out.h>
//
// This code requires the freetype library
// found here: www.freetype.org
//

#ifndef ARDUINO

#include <ctype.h>
#include <ft2build.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../CrossPointFontFormat.h"

#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_TRUETYPE_DRIVER_H
#include "../Group5/g5enc.inl"  // Group5 image compression library
G5ENCIMAGE g5enc;               // Group5 encoder state

#define DPI 150              // Approximate resolution of common displays
#define OUTBUF_SIZE 1048576  // 1MB
#define MAX_INTERVALS 65536
#define FONT_SCALE_FACTOR 2
// TODO: Re-enable small font
// Disabled small font generation to get this working, but want to re-enable
#define SMALL_FONT_ENABLED 0

uint32_t raw_intervals[][2] = {
    /* Basic Latin */
    // ASCII letters, digits, punctuation, control characters
    {0x0000, 0x007F},
    /* Latin-1 Supplement */
    // Accented characters for Western European languages
    {0x0080, 0x00FF},
    /* Latin Extended-A */
    // Eastern European and Baltic languages
    {0x0100, 0x017F},
    /* General Punctuation (core subset) */
    // Smart quotes, en dash, em dash, ellipsis, NO-BREAK SPACE
    {0x2000, 0x206F},
    /* Basic Symbols From "Latin-1 + Misc" */
    // dashes, quotes, prime marks
    {0x2010, 0x203A},
    // misc punctuation
    {0x2040, 0x205F},
    // common currency symbols
    {0x20A0, 0x20CF},
    /* Combining Diacritical Marks (minimal subset) */
    // Needed for proper rendering of many extended Latin languages
    {0x0300, 0x036F},
    /* Greek & Coptic */
    // Used in science, maths, philosophy, some academic texts
    // {0x0370, 0x03FF},
    /* Cyrillic */
    // Russian, Ukrainian, Bulgarian, etc.
    {0x0400, 0x04FF},
    /* Math Symbols (common subset) */
    // Superscripts and Subscripts
    {0x2070, 0x209F},
    // General math operators
    {0x2200, 0x22FF},
    // Arrows
    {0x2190, 0x21FF},
    /* CJK */
    // Core Unified Ideographs
    // {0x4E00, 0x9FFF},
    // Extension A
    // {0x3400, 0x4DBF},
    // Extension B
    // {0x20000, 0x2A6DF},
    // Extension C–F
    // {0x2A700, 0x2EBEF},
    // Extension G
    // {0x30000, 0x3134F},
    // Hiragana
    // {0x3040, 0x309F},
    // Katakana
    // {0x30A0, 0x30FF},
    // Katakana Phonetic Extensions
    // {0x31F0, 0x31FF},
    // Halfwidth Katakana
    // {0xFF60, 0xFF9F},
    // Hangul Syllables
    // {0xAC00, 0xD7AF},
    // Hangul Jamo
    // {0x1100, 0x11FF},
    // Hangul Compatibility Jamo
    // {0x3130, 0x318F},
    // Hangul Jamo Extended-A
    // {0xA960, 0xA97F},
    // Hangul Jamo Extended-B
    // {0xD7B0, 0xD7FF},
    // CJK Radicals Supplement
    // {0x2E80, 0x2EFF},
    // Kangxi Radicals
    // {0x2F00, 0x2FDF},
    // CJK Symbols and Punctuation
    // {0x3000, 0x303F},
    // CJK Compatibility Forms
    // {0xFE30, 0xFE4F},
    // CJK Compatibility Ideographs
    // {0xF900, 0xFAFF},
    /* Specials */
    // Replacement Character
    {0xFFFD, 0xFFFD},
};

//
// Comparison function for qsort
//
int compareIntervals(const void* a, const void* b) {
  const uint32_t* ia = (uint32_t*)a;
  const uint32_t* ib = (uint32_t*)b;
  if (ia[0] < ib[0]) return -1;
  if (ia[0] > ib[0]) return 1;
  return 0;
}

//
// Sort and merge adjacent intervals
// Returns the number of intervals after merging
//
int sortAndMergeIntervals(uint32_t intervals[][2], const int count) {
  int merged_count = 0;

  // Sort intervals by start value
  qsort(intervals, count, sizeof(uint32_t) * 2, compareIntervals);

  // Merge overlapping/adjacent intervals
  for (int i = 0; i < count; i++) {
    if (merged_count > 0 && intervals[i][0] <= intervals[merged_count - 1][1] + 1) {
      // Merge with previous interval
      if (intervals[i][1] > intervals[merged_count - 1][1]) {
        intervals[merged_count - 1][1] = intervals[i][1];
      }
    } else {
      // Add as new interval
      if (merged_count != i) {
        intervals[merged_count][0] = intervals[i][0];
        intervals[merged_count][1] = intervals[i][1];
      }
      merged_count++;
    }
  }

  return merged_count;
}

//
// Create the comments and const array boilerplate for the hex data bytes
//
void StartHexFile(FILE* f, int iLen, const char* fname, int size) {
  int i, j;
  char szTemp[256];
  fprintf(f, "#pragma once\n\n");
  fprintf(f, "//\n// Created with fontconvert, written by Larry Bank, updated for CrossPoint by Dave Allie\n");
  fprintf(f, "// Point size: %d (scaled %dx)\n", size, FONT_SCALE_FACTOR);
  fprintf(f, "// compressed font data size = %d bytes\n//\n", iLen);

  strcpy(szTemp, fname);
  i = strlen(szTemp);
  if (szTemp[i - 2] == '.') szTemp[i - 2] = 0;  // get the leaf name for the data
  j = i;
  // go backwards to get rid trim off just the leaf name
  while (j > 0 && szTemp[j] != '/') {
    j--;
  }
  if (szTemp[j] == '/') j++;
  fprintf(f, "static const uint8_t %s[] = {\n", &szTemp[j]);
} /* StartHexFile() */

//
// Add N bytes of hex data to the output
// The data will be arranged in rows of 16 bytes each
//
void AddHexBytes(FILE* f, void* pData, int iLen, int bLast) {
  static int iCount = 0;  // number of bytes processed so far
  int i;
  uint8_t* s = (uint8_t*)pData;
  for (i = 0; i < iLen; i++) {  // process the given data
    fprintf(f, "0x%02x", *s++);
    iCount++;
    if (i < iLen - 1 || !bLast) fprintf(f, ",");
    if ((iCount & 15) == 0) fprintf(f, "\n");  // next row of 16
  }
  if (bLast) {
    fprintf(f, "};\n");
  }
} /* AddHexBytes() */

int loadCodePoint(FT_Face face, uint32_t code_point, CrossPointFontGlyph* pGlyphs, uint8_t* pBitmap,
                  uint32_t* glyph_index, uint32_t* iOffset) {
  uint8_t* s;
  int iPitch, err;
  FT_Glyph glyph;

  // MONO renderer provides clean image with perfect crop
  // (no wasted pixels) via bitmap struct.
  if ((err = FT_Load_Char(face, code_point, FT_LOAD_TARGET_MONO))) {
    printf("Error %d loading char U+%04X\n", err, code_point);
    (*glyph_index)++;
    return 0;
  }

  if ((err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO))) {
    printf("Error %d rendering char U+%04X\n", err, code_point);
    (*glyph_index)++;
    return 0;
  }

  if ((err = FT_Get_Glyph(face->glyph, &glyph))) {
    printf("Error %d getting glyph U+%04X\n", err, code_point);
    (*glyph_index)++;
    return 0;
  }

  FT_Bitmap* bitmap = &face->glyph->bitmap;
  FT_BitmapGlyphRec* g = (FT_BitmapGlyphRec*)glyph;

  // TODO: Restore small font
  if (0 /* bSmallFont */) {
#if SMALL_FONT_ENABLED == 1
    printf("Stubbed\n");
    return 1;
#else
    printf("Small font has been disabled\n");
    return 1;
#endif
  } else {
    pGlyphs[*glyph_index].bitmapOffset = *iOffset;
    pGlyphs[*glyph_index].width = bitmap->width;
    pGlyphs[*glyph_index].height = bitmap->rows;
    pGlyphs[*glyph_index].xAdvance = (face->glyph->advance.x >> 6);
    pGlyphs[*glyph_index].xOffset = g->left;
    pGlyphs[*glyph_index].yOffset = g->top;
  }
  s = bitmap->buffer;
  iPitch = bitmap->pitch;

  g5_encode_init(&g5enc, bitmap->width, bitmap->rows, &pBitmap[*iOffset], OUTBUF_SIZE - *iOffset);
  for (int y = 0; y < bitmap->rows; y++) {
    g5_encode_encodeLine(&g5enc, &s[y * iPitch]);
  }  // for y
  int iLen = g5_encode_getOutSize(&g5enc);
  *iOffset += iLen;

  FT_Done_Glyph(glyph);
  (*glyph_index)++;
  return 0;
}

int main(int argc, char* argv[]) {
  int i, err, size = 0;
  uint32_t iLen, iOffset = 0;
  FILE* fOut;
  // TrueType library structures
  FT_Library library;
  char* regularFaceFile;
  char* boldFaceFile = NULL;
  char* italicFaceFile = NULL;
  char* boldItalicFaceFile = NULL;
  const char* outputFile = NULL;
  FT_Face faceRegular;
  FT_Face faceBold;
  FT_Face faceItalic;
  FT_Face faceBoldItalic;

  int bSmallFont = 0;  // indicates if we're creating a normal or small font file
  CrossPointFontUnicodeInterval* pIntervals;
  CrossPointFontGlyph* pGlyphs;
#if SMALL_FONT_ENABLED == 1
  CrossPointFontSmallGlyph* pSmallGlyphs;
#endif
  uint8_t* pBitmap;
  CrossPointFontHeader epdFontHeader;

  int bHFile;  // flag indicating if the output will be a .H file of hex data

  // Process intervals
  uint32_t intervals[MAX_INTERVALS][2];
  int intervalCount = sizeof(raw_intervals) / sizeof(raw_intervals[0]);
  uint32_t totalGlyphs = 0;

  if (argc < 6 || argc % 2 == 1) {
    printf(
        "Usage: %s <regular.ttf> [-b <bold.ttf>] [-i <italic.ttf>] [-bi <bold-italic.ttf>] -p point_size -o <out.cpf "
        "or out.h>\n",
        argv[0]);
    return 1;
  }

  regularFaceFile = argv[1];
  for (int i = 2; i < argc; i += 2) {
    if (strcmp(argv[i], "-b") == 0) {
      // Bold font
      boldFaceFile = argv[i + 1];
    } else if (strcmp(argv[i], "-i") == 0) {
      // Italic font
      italicFaceFile = argv[i + 1];
    } else if (strcmp(argv[i], "-bi") == 0) {
      // Bold-Italic font
      boldItalicFaceFile = argv[i + 1];
    } else if (strcmp(argv[i], "-p") == 0) {
      // Point size
      size = atoi(argv[i + 1]);
    } else if (strcmp(argv[i], "-o") == 0) {
      // Output file
      outputFile = argv[i + 1];
      // output an H file?
      bHFile = outputFile[strlen(outputFile) - 1] == 'H' || outputFile[strlen(outputFile) - 1] == 'h';
    } else {
      printf("Unknown argument: %s\n", argv[i]);
      return 1;
    }
  }

  if (!outputFile) {
    printf("No output file specified\n");
    return 1;
  }

  if (size <= 0) {
    printf("Invalid point size: %d\n", size);
    return 1;
  }

  size = size * FONT_SCALE_FACTOR;
  bSmallFont = (size < 60) && SMALL_FONT_ENABLED == 1;  // Glyph info can fit in signed 8-bit values
  int fontVariants = 1;                                 // Always at least one variant we treat as regular
  if (boldFaceFile) fontVariants += 1;
  if (italicFaceFile) fontVariants += 1;
  if (boldItalicFaceFile) fontVariants += 1;

  // Copy and sort/merge intervals
  if (intervalCount > MAX_INTERVALS) {
    printf("Error: too many intervals (max %d)\n", MAX_INTERVALS);
    return 1;
  }
  for (i = 0; i < intervalCount; i++) {
    intervals[i][0] = raw_intervals[i][0];
    intervals[i][1] = raw_intervals[i][1];
  }
  intervalCount = sortAndMergeIntervals(intervals, intervalCount);

  // Calculate total number of glyphs
  for (i = 0; i < intervalCount; i++) {
    totalGlyphs += intervals[i][1] - intervals[i][0] + 1;
  }
  totalGlyphs *= fontVariants;

  printf("Processed intervals: %d, total glyphs: %u\n", intervalCount, totalGlyphs);

  // Allocate memory for intervals
  pIntervals = (CrossPointFontUnicodeInterval*)malloc(intervalCount * sizeof(CrossPointFontUnicodeInterval));
  if (!pIntervals) {
    printf("Error allocating memory for interval data\n");
    return 1;
  }

  // Allocate memory for glyphs
  if (bSmallFont) {
#if SMALL_FONT_ENABLED == 1
    pSmallGlyphs = (CrossPointFontSmallGlyph*)malloc(totalGlyphs * sizeof(CrossPointFontSmallGlyph));
    if (!pSmallGlyphs) {
      printf("Error allocating memory for glyph data\n");
      return 1;
    }
#else
    printf("Small font has been disabled\n");
    return 1;
#endif
  } else {
    pGlyphs = (CrossPointFontGlyph*)malloc(totalGlyphs * sizeof(CrossPointFontGlyph));
    if (!pGlyphs) {
      printf("Error allocating memory for glyph data\n");
      return 1;
    }
  }
  pBitmap = (uint8_t*)malloc(OUTBUF_SIZE);  // Enough to hold the output
  if (!pBitmap) {
    printf("Error allocating memory for bitmap data\n");
    return 1;
  }

  // Init FreeType lib, load font
  if ((err = FT_Init_FreeType(&library))) {
    printf("FreeType init error: %d", err);
    return err;
  }

  // Use TrueType engine version 35, without subpixel rendering.
  // This improves clarity of fonts since this library does not
  // support rendering multiple levels of gray in a glyph.
  // See https://github.com/adafruit/Adafruit-GFX-Library/issues/103
  FT_UInt interpreter_version = TT_INTERPRETER_VERSION_35;
  FT_Property_Set(library, "truetype", "interpreter-version", &interpreter_version);

  if ((err = FT_New_Face(library, regularFaceFile, 0, &faceRegular))) {
    printf("Font load error: %d\n", err);
    FT_Done_FreeType(library);
    return err;
  }

  if (italicFaceFile && (err = FT_New_Face(library, italicFaceFile, 0, &faceItalic))) {
    printf("Font load error: %d\n", err);
    FT_Done_FreeType(library);
    return err;
  }

  if (boldFaceFile && (err = FT_New_Face(library, boldFaceFile, 0, &faceBold))) {
    printf("Font load error: %d\n", err);
    FT_Done_FreeType(library);
    return err;
  }

  if (boldItalicFaceFile && (err = FT_New_Face(library, boldItalicFaceFile, 0, &faceBoldItalic))) {
    printf("Font load error: %d\n", err);
    FT_Done_FreeType(library);
    return err;
  }

  // Shift the size left by 6 because the library uses '26dot6' fixed-point format
  FT_Set_Char_Size(faceRegular, size << 6, 0, DPI, 0);
  if (boldFaceFile) FT_Set_Char_Size(faceBold, size << 6, 0, DPI, 0);
  if (italicFaceFile) FT_Set_Char_Size(faceItalic, size << 6, 0, DPI, 0);
  if (boldItalicFaceFile) FT_Set_Char_Size(faceBoldItalic, size << 6, 0, DPI, 0);

  // Build intervals with offsets and process glyphs
  uint32_t glyph_index = 0;
  for (int iInterval = 0; iInterval < intervalCount; iInterval++) {
    const uint32_t intervalStart = intervals[iInterval][0];
    const uint32_t intervalEnd = intervals[iInterval][1];

    // Store interval with offset
    pIntervals[iInterval].first = intervalStart;
    pIntervals[iInterval].last = intervalEnd;
    pIntervals[iInterval].offset = glyph_index;

    // Process each glyph in this interval
    // Load the codepoint for each style variant
    for (uint32_t codePoint = intervalStart; codePoint <= intervalEnd; codePoint++) {
      loadCodePoint(faceRegular, codePoint, pGlyphs, pBitmap, &glyph_index, &iOffset);
      if (boldFaceFile) loadCodePoint(faceBold, codePoint, pGlyphs, pBitmap, &glyph_index, &iOffset);
      if (italicFaceFile) loadCodePoint(faceItalic, codePoint, pGlyphs, pBitmap, &glyph_index, &iOffset);
      if (boldItalicFaceFile) loadCodePoint(faceBoldItalic, codePoint, pGlyphs, pBitmap, &glyph_index, &iOffset);
    }  // for each code point in interval
  }  // for each interval

  // Try to create the output file
  fOut = fopen(outputFile, "w+b");
  if (!fOut) {
    printf("Error creating output file: %s\n", outputFile);
    return 1;
  }

  epdFontHeader.height = faceRegular->size->metrics.height >> 6;
  epdFontHeader.ascender = faceRegular->size->metrics.ascender >> 6;
  epdFontHeader.styles = 0b0001;
  if (boldFaceFile) epdFontHeader.styles |= 0b0010;
  if (italicFaceFile) epdFontHeader.styles |= 0b0100;
  if (boldItalicFaceFile) epdFontHeader.styles |= 0b1000;
  epdFontHeader.intervalCount = intervalCount;
  epdFontHeader.glyphCount = totalGlyphs;

  // Write the file header
  if (bSmallFont) {
#if SMALL_FONT_ENABLED == 1
    epdFontHeader.u16Marker = CPF_FONT_MARKER_SMALL;
    if (faceRegular->size->metrics.height == 0) {
      // No face height info, assume fixed width and get from a glyph.
      epdFontHeader.height = pSmallGlyphs[0].height;
    }

    iLen = sizeof(CrossPointFontHeader) + intervalCount * sizeof(CrossPointFontUnicodeInterval) +
           totalGlyphs * sizeof(CrossPointFontSmallGlyph) + iOffset;
    if (bHFile) {  // create an H file of hex values
      StartHexFile(fOut, iLen, outputFile, size);
      AddHexBytes(fOut, &epdFontHeader, sizeof(CrossPointFontHeader), 0);
      // Write the intervals
      AddHexBytes(fOut, pIntervals, sizeof(CrossPointFontUnicodeInterval) * intervalCount, 0);
      // Write the glyph table
      AddHexBytes(fOut, pSmallGlyphs, sizeof(CrossPointFontSmallGlyph) * totalGlyphs, 0);
      // Write the compressed bitmap data
      AddHexBytes(fOut, pBitmap, iOffset, 1);
    } else {
      fwrite(&epdFontHeader, 1, sizeof(CrossPointFontHeader), fOut);
      // Write the intervals
      fwrite(pIntervals, 1, intervalCount * sizeof(CrossPointFontUnicodeInterval), fOut);
      // Write the glyph table
      fwrite(pSmallGlyphs, 1, totalGlyphs * sizeof(CrossPointFontSmallGlyph), fOut);
      // Write the compressed bitmap data
      fwrite(pBitmap, 1, iOffset, fOut);
    }
#else
    printf("Small font has been disabled\n");
    return 1;
#endif
  } else {
    epdFontHeader.u16Marker = CPF_FONT_MARKER;
    if (faceRegular->size->metrics.height == 0) {
      // No face height info, assume fixed width and get from a glyph.
      epdFontHeader.height = pGlyphs[0].height;
    }

    iLen = sizeof(CrossPointFontHeader) + intervalCount * sizeof(CrossPointFontUnicodeInterval) +
           totalGlyphs * sizeof(CrossPointFontGlyph) + iOffset;
    if (bHFile) {  // create an H file of hex values
      StartHexFile(fOut, iLen, outputFile, size);
      AddHexBytes(fOut, &epdFontHeader, sizeof(CrossPointFontHeader), 0);
      // Write the intervals
      AddHexBytes(fOut, pIntervals, sizeof(CrossPointFontUnicodeInterval) * intervalCount, 0);
      // Write the glyph table
      AddHexBytes(fOut, pGlyphs, sizeof(CrossPointFontGlyph) * totalGlyphs, 0);
      // Write the compressed bitmap data
      AddHexBytes(fOut, pBitmap, iOffset, 1);
    } else {
      fwrite(&epdFontHeader, 1, sizeof(CrossPointFontHeader), fOut);
      // Write the intervals
      fwrite(pIntervals, 1, intervalCount * sizeof(CrossPointFontUnicodeInterval), fOut);
      // Write the glyph table
      fwrite(pGlyphs, 1, totalGlyphs * sizeof(CrossPointFontGlyph), fOut);
      // Write the compressed bitmap data
      fwrite(pBitmap, 1, iOffset, fOut);
    }
  }  // large fonts
  fflush(fOut);
  fclose(fOut);  // done!
  FT_Done_FreeType(library);
  printf("Success!\nFont file size: %d bytes (%d glyphs)\n", iLen, totalGlyphs);

  return 0;
} /* main() */

#endif
