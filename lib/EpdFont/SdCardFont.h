#pragma once

#include <cstdint>

#include "EpdFont.h"
#include "EpdFontData.h"

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;

  SdCardFont() = default;
  ~SdCardFont();

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from utf8Text (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles, restore stub EpdFontData.
  void clearCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;

    // Persistent kern/ligature data (lazy-loaded on first prewarm)
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    int8_t* kernMatrix = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  static constexpr uint32_t OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  void clearAdvanceTables();

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
