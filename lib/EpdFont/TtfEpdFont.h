#pragma once

// CrossPoint <- FreeInkFont adapter (FreeType, 4-style, lazy, streaming).
//
// Renders one or more TrueType/OpenType files — including OpenType VARIABLE
// fonts — as a CrossPoint EpdFontFamily with regular / bold / italic /
// bold-italic via the FreeInkFont FreeType backend.
//
// STYLE SOURCES. A family can be built from up to four source files, one per
// style role (0=regular, 1=bold, 2=italic, 3=bold-italic); only regular is
// required. Each face resolves to the best available source:
//   * an explicit file for that style, if supplied (e.g. a separate
//     Family-Italic.ttf → true italic letterforms, or a static Family-Bold.ttf);
//   * else the regular source with the wght axis pushed to bold (variable
//     fonts) or a per-glyph outline embolden (static) for bold;
//   * else the regular source with an ital/slnt axis or an oblique shear for
//     italic. Bold-italic combines the two, preferring an italic source's bold.
// So a single variable file yields all four styles, and dropping in dedicated
// Bold/Italic files upgrades those styles to the real designs.
//
// Memory-lean:
//   * STREAMING (addStreamSource) — FreeType pulls bytes from SD on demand, so a
//     multi-MB variable/CJK file never sits in RAM. Use addResidentSource() only
//     for small fonts already held in RAM.
//   * LAZY per-style faces — only the regular face is built up front; bold /
//     italic / bold-italic (and their glyph caches) are created on first use, so
//     a book with no bold pays nothing for it.
//   * per-face glyph caches that GROW TO CONVERGE (no worst-case pre-reserve):
//     the byte arena and glyph tables start empty and grow only to the book's
//     actual page needs, then stop touching the allocator (clearCache keeps the
//     capacity across page turns). Bounded by a hard byte cap that flushes when
//     full, and shed entirely by releaseResidentCaches() on heap-critical
//     transitions — on par with the SD (.cpfont) font system's discipline.
//   * caches live in PSRAM when the board has it (FontPsram / FontAlloc), so
//     glyph arenas don't consume scarce internal SRAM.
//
// Lifetime: every configured source (resident bytes or streamed read source,
// e.g. an open SD file) is BORROWED and must outlive this object, which must
// outlive any GfxRenderer registration.

#include "VectorFontSupport.h"

#if CROSSPOINT_VECTOR_FONTS

#include <FontPsram.h>
#include <FtFont.h>

#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"

class TtfEpdFont {
 public:
  // Style roles for source slots.
  enum Style : uint8_t { Regular = 0, Bold = 1, Italic = 2, BoldItalic = 3 };

  // Configure a source file for a style role BEFORE calling load(). Regular is
  // required; the rest are optional and upgrade their style to a real design.
  // Resident: bytes borrowed. Streamed: read source borrowed.
  void addResidentSource(uint8_t style, const uint8_t* data, uint32_t len);
  void addStreamSource(uint8_t style, freeink::font::FtFont::ReadFn read, void* ctx, unsigned long fileSize);

  // Build the family at the given reader point size from the configured sources.
  // Returns false if the regular source is missing or unparseable.
  bool load(uint16_t pointSize, bool twoBit = true, size_t glyphCacheBytes = 32 * 1024, uint16_t maxGlyphs = 768);

  EpdFontFamily family() const;

  // Per-scope reset (mirrors SdCardFont::clearCache): drop every face's cached
  // page glyphs but KEEP the allocations (byte arena + vector capacity), so a
  // page turn re-faults into buffers already sized to the book and stops touching
  // the allocator once converged. Driven by FontCacheManager::clearCache() /
  // PrewarmScope, symmetrically with the SD fonts.
  void clearCache();

  // Heap-critical teardown (mirrors SdCardFont::releaseResidentCaches): free
  // every rebuildable cache — the byte arenas, glyph tables, and the lazy bold/
  // italic/bold-italic FreeType faces — keeping only the regular face live so
  // coverage() still answers without a reload. Everything faults back in on
  // demand. Driven by FontCacheManager::releaseSdFontCaches() before heap-hungry
  // transitions (WiFi + web server, image decode, dictionary, sleep).
  void releaseResidentCaches();

  // Optional batch pre-warm of the REGULAR face (other styles fault lazily).
  bool build(const char* utf8);
  bool addCoverage(const char* utf8);

 private:
  // A borrowed source file (one per style role that the caller supplies).
  struct Source {
    bool present = false;
    bool streamed = false;
    const uint8_t* data = nullptr;  // resident form
    uint32_t len = 0;
    freeink::font::FtFont::ReadFn read = nullptr;  // streamed form
    void* ctx = nullptr;
    unsigned long fileSize = 0;
  };

  struct Face {
    TtfEpdFont* owner = nullptr;
    freeink::font::FtFont ft;
    freeink::font::PsramVector<uint8_t> bmp;
    size_t used = 0;
    size_t cap = 0;
    uint16_t maxGlyphs = 0;
    bool twoBit = true;
    uint16_t sizePx = 0;
    uint8_t srcIndex = 0;     // which Source this face initializes from
    int weight = 400;         // design weight requested (wght axis / faux bold)
    bool wantItalic = false;  // request italic from the source (axis or oblique)
    bool inited = false;      // init attempted (lazy)
    bool ready = false;       // FreeType face live
    // GSUB 'liga' resolution (fi fl ff ffi ffl), resolved once at initFace():
    // ligGid[i] is the raw glyph ID for U+FB00+i (0 = none), and ligPairs is
    // the EpdFont pair table (sorted by key) that routes applyLigatures() to
    // those presentation codepoints; faultGlyph() then rasterizes them by
    // glyph ID even when the face's cmap has no entry for them.
    uint32_t ligGid[5] = {0, 0, 0, 0, 0};
    EpdLigaturePair ligPairs[5] = {};
    uint8_t ligPairCount = 0;
    freeink::font::PsramVector<EpdGlyph> glyphs;
    freeink::font::PsramVector<uint32_t> cps;
    freeink::font::PsramVector<uint16_t> slot;
    // Kern pair cache ((left<<32)|right → 4.4 value), sorted by key. Kerning
    // resolves through FtFont (legacy 'kern' table, then the GPOS 'kern'
    // feature) once per pair, then serves from here — getKerning runs for
    // every adjacent glyph pair on every draw/measure pass.
    freeink::font::PsramVector<uint64_t> kernKeys;
    freeink::font::PsramVector<int8_t> kernVals;
    EpdFontData data{};
    EpdFont font{&data};
  };

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static const uint8_t* bitmapThunk(void* ctx, const EpdGlyph* glyph);
  static bool coverageThunk(void* ctx, uint32_t codepoint);
  static int8_t kernThunk(void* ctx, uint32_t leftCp, uint32_t rightCp);

  void resolveFaces();             // map the 4 faces onto the configured sources
  void initFace(Face& f);          // lazy: create the FT face on first use
  void setupFace(Face& f);         // wire data handlers + metrics
  void resolveLigatures(Face& f);  // query GSUB once, build ligGid/ligPairs
  const EpdGlyph* faultGlyph(Face& f, uint32_t codepoint);
  int8_t faultKern(Face& f, uint32_t leftCp, uint32_t rightCp);
  static void flushFace(Face& f);

  Source sources_[4];      // indexed by Style role
  Face faces_[4];          // 0=regular 1=bold 2=italic 3=bold-italic
  uint32_t size26_6_ = 0;  // exact 26.6 ppem (pt @150DPI), no whole-pixel rounding
  bool loaded_ = false;
  // Set while a glyph fault runs MemoryManager::ensureFree(): the eviction
  // sink calls releaseResidentCaches() on every TTF font, and tearing down
  // the very faces mid-fault would be use-after-free.
  bool evictionLocked_ = false;
};

#endif  // CROSSPOINT_VECTOR_FONTS
