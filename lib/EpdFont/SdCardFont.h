#pragma once

#include "EpdFont.h"
#include "EpdFontData.h"

#include <cstdint>

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;

  SdCardFont() = default;
  ~SdCardFont();

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // Builds a mini EpdFontData containing only those glyphs.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data) —
  // useful for layout/measurement passes where only advanceX is needed.
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, bool metadataOnly = false);

  // Free mini data, restore stub EpdFontData.
  void clearCache();

  // Returns pointer to the managed EpdFont (data pointer swapped by prewarm/clearCache).
  EpdFont* getEpdFont() { return &epdFont_; }

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

 private:
  // .cpfont header fields
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    // v2 fields (kern/ligature)
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  CpFontHeader header_{};
  char filePath_[128] = {};

  // File layout offsets
  uint32_t intervalsFileOffset_ = 0;
  uint32_t glyphsFileOffset_ = 0;
  uint32_t kernLeftFileOffset_ = 0;
  uint32_t kernRightFileOffset_ = 0;
  uint32_t kernMatrixFileOffset_ = 0;
  uint32_t ligatureFileOffset_ = 0;
  uint32_t bitmapFileOffset_ = 0;

  // Full intervals loaded from file (kept in RAM for codepoint lookup)
  EpdUnicodeInterval* fullIntervals_ = nullptr;

  // Persistent kern/ligature data (lazy-loaded on first prewarm(), freed in freeAll())
  EpdKernClassEntry* kernLeftClasses_ = nullptr;
  EpdKernClassEntry* kernRightClasses_ = nullptr;
  int8_t* kernMatrix_ = nullptr;
  EpdLigaturePair* ligaturePairs_ = nullptr;
  bool kernLigLoaded_ = false;

  // Stub EpdFontData returned when not prewarmed (empty intervals = all lookups return nullptr)
  EpdFontData stubData_{};

  // Mini EpdFontData built during prewarm
  EpdFontData miniData_{};
  EpdUnicodeInterval* miniIntervals_ = nullptr;
  EpdGlyph* miniGlyphs_ = nullptr;
  uint8_t* miniBitmap_ = nullptr;
  uint32_t miniIntervalCount_ = 0;
  uint32_t miniGlyphCount_ = 0;

  // The EpdFont whose data pointer we manage
  EpdFont epdFont_{&stubData_};

  Stats stats_;
  bool loaded_ = false;

  void freeMiniData();
  void freeAll();
  void freeKernLigatureData();
  bool loadKernLigatureData();
  void applyKernLigaturePointers(EpdFontData& data) const;
  int32_t findGlobalGlyphIndex(uint32_t codepoint) const;
};
