#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "GlyphProvider.h"

class HalFile;
class BuildArena;

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;
  // prewarmStyle: the bitmap arena did not fit in the largest free block. Distinct from a
  // missed-glyph count so prewarm() can tell "the heap is fragmented, try a smaller set"
  // apart from "these glyphs are not in the font", and retry rather than give up.
  static constexpr int PREWARM_ARENA_TOO_LARGE = -2;

  SdCardFont() = default;
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;
  ~SdCardFont();

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Load from a flash partition mmap pointer (e.g. from FlashFontPartition::mmap()).
  // fullIntervals and kern/lig tables are read directly from the mmap region — no
  // heap allocation for metadata.  The mmap pointer must remain valid for the
  // lifetime of this SdCardFont.  unloadMetadata() and reloadMetadata() are no-ops
  // for mmap-loaded fonts (metadata is always present in the flash mapping).
  // sdPath must be the SD path of the same .cpfont — it is used by the bitmap
  // overflow handler to load glyph bitmaps from SD at draw time.
  // Returns true on success.
  bool loadFromMmap(const uint8_t* base, size_t size, const char* sdPath);

  // Serve every array this font allocates from `arena` instead of the heap, except the glyph-miss
  // ring's bitmaps (OverflowEntry::bitmap). Nothing in the arena is freed one array at a time; the
  // caller reclaims it all by rewinding the arena AFTER this font is destroyed.
  // Call before load()/loadFromMmap() and never change it afterwards: an array has to be freed the
  // way it was allocated. The font selector's previews use it (FontSelectionActivity::
  // loadPreviewFont); the reader never does.
  void useArena(BuildArena* arena) { arena_ = arena; }

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // If loadKernLigatureData=true, kern/ligature metadata is also loaded so layout
  // measurement calls that use applyLigatures()/getKerning() produce correct results.
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false,
              bool loadKernLigatureData = false);

  // Free mini data for all styles, restore stub EpdFontData.
  void clearCache();

  // Soft cache reset: drop the cumulative metadata-only prewarm cache built
  // up by repeated layout-time ensureFontReady() calls. Bitmap-mode
  // (FULL) caches are also dropped. Call this between sections to bound the
  // cumulative cp set growth across pagination.
  void clearAccumulation();

  // Phase lifecycle: drop all layout-phase metadata (fullIntervals + kern/lig tables)
  // to free ~40–50 KB before createSectionFile(). File offsets are preserved so
  // reloadMetadata() can restore everything without re-reading the file header.
  // Mini buffers are NOT affected — those are already managed per-section.
  // No-op if not loaded.
  void unloadMetadata();

  // Restore layout-phase metadata after createSectionFile() completes.
  // Re-reads fullIntervals and kern/lig tables from SD using stored file offsets.
  // Returns true on success; false leaves the font in a degraded (stub-only) state.
  bool reloadMetadata();

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
    // Index of the earlier style whose fullIntervals this style borrows, or -1 when the
    // table is this style's own allocation. Regular/bold/italic weights of one family
    // almost always cover the same codepoints, so the tables are byte-identical -- on a
    // CJK font that is up to 48 KB per style. Only the owner frees; a borrower survives
    // unloadMetadata() so reloadMetadata() can point it at the owner's new table.
    int8_t intervalsOwner = -1;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool ligLoaded = false;          ///< ligaturePairs resident
    bool kernClassesLoaded = false;  ///< kernLeft/RightClasses resident (skipped during metadata-only prewarm)

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    // Bitmap bytes per glyph of the last requested set, rounded up, for sizing the arena
    // retry. Rounded rather than floored: the retry multiplies it back out, and a floored
    // figure yields a glyph count whose arena still does not fit.
    uint32_t measuredBytesPerGlyph = 0;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Cache mode for the current mini buffers:
    //   NONE     = empty / freshly cleared, no glyphs loaded
    //   METADATA = miniGlyphs has glyph metrics only (no bitmap data); built
    //              up incrementally across paragraph-level ensureFontReady
    //              calls during pagination. Safe to merge new cps into.
    //   FULL     = miniGlyphs + miniBitmap loaded, page-scoped (built once per
    //              page render). Layout-only prewarm calls are no-ops in this
    //              mode (overflow handler covers any uncovered cp at draw time).
    enum class MiniMode : uint8_t { NONE, METADATA, FULL };
    MiniMode miniMode = MiniMode::NONE;

    // Codepoints already reported as missing during the current accumulation
    // cycle. Cleared by clearAccumulation() / freeStyleAll(). Bounded by
    // MAX_REPORTED_MISSES; once full, further misses go unreported (the cps
    // still cleanly fall back to the replacement glyph in EpdFont::getGlyph).
    static constexpr uint8_t MAX_REPORTED_MISSES = 32;
    uint32_t reportedMisses[MAX_REPORTED_MISSES] = {};
    uint8_t reportedMissCount = 0;

    // Debug counters for on-demand glyph misses (StyleProvider::missGlyph). Used to
    // throttle log volume while still preserving visibility into overflow churn.
    uint32_t onDemandMissCount = 0;
    uint16_t onDemandMissLogged = 0;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Each style's EpdFontData points at its own StyleProvider: a miss goes to the shared ring, and
  // the renderer asks it whether a glyph's bitmap is one the ring holds.
  class StyleProvider final : public GlyphProvider {
   public:
    SdCardFont* self = nullptr;
    uint8_t styleIdx = 0;

    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return SdCardFont::onGlyphMiss(this, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      if (!self->isOverflowGlyph(glyph)) return false;
      bitmap = self->getOverflowBitmap(glyph);
      return true;
    }
  };
  StyleProvider styleProviders_[MAX_STYLES];

  // Shared on-demand overflow buffer (ring of glyphs loaded by StyleProvider::missGlyph)
  static constexpr uint32_t OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    // Always on the heap, even with an arena (useArena): the ring frees one slot at a time, which a
    // bump arena cannot, so every eviction would strand its bitmap there until the block is rewound.
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
    bool occupied = false;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // True when persistent metadata (fullIntervals, kernLeft/RightClasses,
  // ligaturePairs) is heap-allocated and must be delete[]'d on free.
  // False when loaded via loadFromMmap() — those pointers alias the flash
  // partition mmap region and must never be freed.
  bool metadataOwned_ = true;

  // Base pointer of the mmap'd .cpfont data (the value passed to loadFromMmap).
  // Non-null only when metadataOwned_ == false. Used to read sections (e.g. the
  // kern matrix) directly from flash without SD I/O.
  const uint8_t* mmapDataBase_ = nullptr;

  // See useArena(). Null: every allocation below is the plain heap call it replaced.
  BuildArena* arena_ = nullptr;
  template <typename T>
  T* allocArray(size_t count);
  template <typename T>
  void freeArray(T* p);
  // A buffer that lives for one call. On the heap it is freed when the scope ends; in an arena it
  // stays until the caller rewinds the font's block, so the deleter does nothing.
  template <typename T>
  struct ScratchDeleter {
    bool heap = true;
    void operator()(T* p) const {
      if (heap) delete[] p;
    }
  };
  template <typename T>
  using Scratch = std::unique_ptr<T[], ScratchDeleter<T>>;
  template <typename T>
  Scratch<T> makeScratch(size_t count);
  // The biggest single allocation the prewarm bitmap retry can make: the arena's remaining room,
  // or the heap's largest free block.
  uint32_t maxAllocatable() const;

  // Per-style helpers
  static bool allCpsCovered(const PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void freeStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s, bool ligatureOnly = false);
  int8_t findIdenticalIntervals(uint8_t styleIdx, HalFile& file);
  int8_t findIdenticalIntervals(uint8_t styleIdx, const uint8_t* records);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount, HalFile& file);
  void applyKernLigaturePointers(const PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                   bool loadKernLigatureData);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // The miss path behind StyleProvider::missGlyph; ctx is that StyleProvider.
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
