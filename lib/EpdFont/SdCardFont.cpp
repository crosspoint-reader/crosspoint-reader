#include "SdCardFont.h"

#include <FontPsram.h>  // PSRAM-preferring resident buffers (font memory lift)
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

// Resident SD-font buffers (glyph/kern arenas, interval + advance tables, the
// overflow ring) are placed in PSRAM when the board has it — freeing scarce
// internal SRAM — via these helpers. On a no-PSRAM board they fall back to the
// same internal heap `new[]`/`delete[]` used before, so behavior (and the
// fragmentation-avoidance logic below) is unchanged there. Every buffer routed
// through psramNewArray MUST be released with psramDeleteArray.
using freeink::font::psramDeleteArray;
using freeink::font::psramNewArray;

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// resetStyleMiniData retention bounds (see the PerStyle comment in the header).
constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40 * 1024;
constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;
// Working headroom left outside the mini bitmap arena's single contiguous block.
constexpr uint32_t PREWARM_MAX_ALLOC_RESERVE = 4 * 1024;

// Keep-if-fits buffer reuse: only reallocate when the needed size exceeds the
// current capacity. Freeing + reallocating slightly different sizes every page
// turn punches non-coalescing holes in the heap (the freed block rarely fits the
// next page's need), eroding the largest contiguous block all session. With
// reuse, capacities converge on the book's max page after a few turns and page
// turns stop touching the allocator. Only three small instantiations exist
// (interval/glyph/byte arrays), so template bloat is negligible.
template <typename T, typename CapT>
bool ensureArrayCapacity(T*& buf, CapT& capacity, const uint32_t needed) {
  if (buf && capacity >= needed) return true;
  psramDeleteArray(buf);
  buf = psramNewArray<T>(needed > 0 ? needed : 1);
  capacity = buf ? static_cast<CapT>(needed) : 0;
  return buf != nullptr;
}

}  // namespace

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  psramDeleteArray(s.miniIntervals);
  s.miniIntervals = nullptr;
  psramDeleteArray(s.miniGlyphs);
  s.miniGlyphs = nullptr;
  psramDeleteArray(s.miniBitmap);
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s) {
  // Retention is a bet that the next scope needs similar data. Don't hold it
  // when the heap is tight: the arenas are rebuildable for one page's worth of
  // allocations, and this floor keeps retained fonts out of the way of section
  // builds and the render path's own floors.
  if (ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP) {
    freeStyleMiniData(s);
    return;
  }
  // Data (intervals/glyphs/bitmaps/kern) deliberately survives the scope: the
  // next prewarm subset-checks against it, which is what lets the idle prewarm
  // of page N+1 serve the actual page turn with zero SD reads.
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  // Both font views borrow the resident ligature table.
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
  psramDeleteArray(s.kernLeftClasses);
  s.kernLeftClasses = nullptr;
  psramDeleteArray(s.kernRightClasses);
  s.kernRightClasses = nullptr;
  psramDeleteArray(s.ligaturePairs);
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  psramDeleteArray(s.miniKernLeftClasses);
  s.miniKernLeftClasses = nullptr;
  psramDeleteArray(s.miniKernRightClasses);
  s.miniKernRightClasses = nullptr;
  psramDeleteArray(s.miniKernMatrix);
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernLeftCapacity = 0;
  s.miniKernRightCapacity = 0;
  s.miniKernMatrixCapacity = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  // An earlier style owns any shared interval table.
  if (!s.intervalsShared) {
    psramDeleteArray(s.fullIntervals);
    psramDeleteArray(s.bmpIntervals);
  }
  s.fullIntervals = nullptr;
  s.bmpIntervals = nullptr;
  s.intervalsShared = false;
  s.intervalsAreBmp16 = false;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::releaseResidentCaches(bool preserveAdvances) {
  clearOverflow();
  if (!preserveAdvances) clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);  // also frees mini kern and restores the stub EpdFontData
    freeStyleKernLigatureData(styles_[i]);
    applyGlyphMissCallback(i);  // keep the on-demand miss path alive on the stub
  }
}

void SdCardFont::freeAll() {
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  psramDeleteArray(uniformAdvances_);
  uniformAdvances_ = nullptr;
  std::fill_n(uniformAdvanceEnd_, MAX_STYLES, 0);
  uniformAdvancesScanned_ = false;
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
  advancesPrepared_ = false;
#if LOG_LEVEL >= 2
  std::fill_n(advanceDirectReads_, MAX_STYLES, 0);
  std::fill_n(advanceFullMisses_, MAX_STYLES, 0);
  std::fill_n(advancePeak_, MAX_STYLES, 0);
#endif
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    psramDeleteArray(overflow_[i].bitmap);
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  // Packed class maps and dense matrix, as stored in the .cpfont and mapped in place; the split
  // and sparse forms are built-in only. Set explicitly rather than relying on the caller's
  // initialisation: getKerning() picks the representation by which pointer is non-null.
  data.kernLeftCodepoints = nullptr;
  data.kernLeftClassIds = nullptr;
  data.kernRightCodepoints = nullptr;
  data.kernRightClassIds = nullptr;
  data.kernRowOffsets = nullptr;
  data.kernSparseCols = nullptr;
  data.kernSparseValues = nullptr;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    s.kernLeftClasses = psramNewArray<EpdKernClassEntry>(s.header.kernLeftEntryCount);
    s.kernRightClasses = psramNewArray<EpdKernClassEntry>(s.header.kernRightEntryCount);

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = psramNewArray<EpdLigaturePair>(s.header.ligaturePairCount);
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  // No freeStyleMiniKern here: it zeroed the capacities, which forced the
  // ensureArrayCapacity calls below to reallocate every page and defeated the
  // buffer reuse. prewarmStyle is the only caller and the success path
  // overwrites the contents and all four counts, so keeping the buffers is
  // safe. The early returns zero the counts (buffers kept) so a page with no
  // applicable kern pairs kerns as none instead of through the previous
  // page's tables.
  const auto resetMiniKernCounts = [&s]() {
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
  };
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    resetMiniKernCounts();
    return true;  // font has no kern classes — nothing to build
  }

  // Step 1: mark used left/right classes via a 256-wide bitmap (class IDs are uint8_t).
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = true;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = true;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    resetMiniKernCounts();
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: size the three mini buffers (reused across pages when they fit; the
  // per-page sizes vary by a few entries, which as free+realloc churn was punching
  // non-coalescing holes in the heap every page turn).
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  if (!ensureArrayCapacity(s.miniKernLeftClasses, s.miniKernLeftCapacity, miniLeftCount) ||
      !ensureArrayCapacity(s.miniKernRightClasses, s.miniKernRightCapacity, miniRightCount) ||
      !ensureArrayCapacity(s.miniKernMatrix, s.miniKernMatrixCapacity, matrixBytes)) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata).
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }

  std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[s.header.kernRightClassCount]);
  if (!rowBuf) {
    LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", s.header.kernRightClassCount);
    freeStyleMiniKern(s);
    return false;
  }

  for (uint8_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), s.header.kernRightClassCount) !=
        static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint8_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.coverageHandler = &SdCardFont::onCoverageQuery;
}

bool SdCardFont::onCoverageQuery(void* ctx, const uint32_t codepoint) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return false;  // coverage index freed/never loaded
  return octx->self->findGlobalGlyphIndex(s, codepoint) >= 0;
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style. BMP-only fonts with
  // fewer than 65536 glyphs use a compact 6-byte interval table instead of the
  // on-disk 12-byte table; large sparse CJK subsets otherwise keep tens of KB
  // of always-resident heap just for lookup metadata.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};

    // Regular/bold/italic weights of the same family almost always cover the identical codepoint
    // set, so a later style's table is usually a byte-for-byte copy of an earlier one's. Sharing
    // it saves a full table per style, which on a broad CJK font is tens of KB, and saves the
    // PEAK rather than just the residency: allocating first and de-duplicating afterwards still
    // needs both tables at once, and that peak is what fails on a tight heap.
    // The decision rides along with the validation read below -- every record is already being
    // read here -- so it costs no second pass over the table and no buffer to hold one.
    // A style stays a candidate only while its table has matched every record so far.
    uint8_t shareCandidates = 0;
    for (uint8_t k = 0; k < i; k++) {
      const auto& owner = styles_[k];
      if (!owner.present || owner.header.intervalCount != s.header.intervalCount) continue;
      if (!owner.bmpIntervals && !owner.fullIntervals) continue;
      shareCandidates |= static_cast<uint8_t>(1u << k);
    }
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
        canUseBmp16 = false;
      }
      for (uint8_t k = 0; k < i && shareCandidates != 0; k++) {
        if ((shareCandidates & (1u << k)) == 0) continue;
        const auto& owner = styles_[k];
        // Compared by value, so an above-BMP record never equals a compact one and drops out here.
        const bool same = owner.intervalsAreBmp16
                              ? (owner.bmpIntervals[j].first == iv.first && owner.bmpIntervals[j].last == iv.last &&
                                 owner.bmpIntervals[j].offset == iv.offset)
                              : (owner.fullIntervals[j].first == iv.first && owner.fullIntervals[j].last == iv.last &&
                                 owner.fullIntervals[j].offset == iv.offset);
        if (!same) shareCandidates &= static_cast<uint8_t>(~(1u << k));
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    // Survived every record: alias the earlier style's table instead of allocating a copy.
    // freeStyleAll() releases the table through its owning style.
    for (uint8_t k = 0; k < i && shareCandidates != 0; k++) {
      if ((shareCandidates & (1u << k)) == 0) continue;
      auto& owner = styles_[k];
      // Identical content can still be held in the other resident form when the two styles
      // disagree on glyph count; aliasing across forms would misread the table.
      if (owner.intervalsAreBmp16 != canUseBmp16) continue;
      s.bmpIntervals = owner.bmpIntervals;
      s.fullIntervals = owner.fullIntervals;
      s.intervalsAreBmp16 = owner.intervalsAreBmp16;
      s.intervalsShared = true;
      LOG_DBG("SDCF", "Style %u: sharing style %u's %u-interval table (%u B not allocated)", i, k,
              s.header.intervalCount,
              s.header.intervalCount * (canUseBmp16 ? 6u : static_cast<uint32_t>(sizeof(EpdUnicodeInterval))));
      break;
    }

    // Only the allocate-and-read path below needs the records again; a shared style is done.
    if (!s.intervalsShared && !file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (s.intervalsShared) {
      // Aliased above; fall through to the stub/metadata setup without touching the table.
    } else if (canUseBmp16) {
      s.bmpIntervals = psramNewArray<PerStyle::BmpInterval16>(s.header.intervalCount);
      if (!s.bmpIntervals) {
        LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
        freeAll();
        return false;
      }
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
          LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
          freeAll();
          return false;
        }
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      s.intervalsAreBmp16 = true;
    } else {
      s.fullIntervals = psramNewArray<EpdUnicodeInterval>(s.header.intervalCount);
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        freeAll();
        return false;
      }
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint, uint16_t* intervalIndex) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const uint32_t first = s.intervalsAreBmp16 ? s.bmpIntervals[mid].first : s.fullIntervals[mid].first;
    const uint32_t last = s.intervalsAreBmp16 ? s.bmpIntervals[mid].last : s.fullIntervals[mid].last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = s.intervalsAreBmp16 ? s.bmpIntervals[mid].offset : s.fullIntervals[mid].offset;
      if (intervalIndex) *intervalIndex = static_cast<uint16_t>(mid);
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Prewarm ---

namespace {
const char* singleTextGetter(const void* ctx, uint32_t) { return static_cast<const char*>(ctx); }
}  // namespace

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, bool loadKernLig, bool accumulate) {
  return prewarm(&singleTextGetter, utf8Text, 1, styleMask, metadataOnly, loadKernLig, accumulate);
}

int SdCardFont::prewarm(TextGetter getter, const void* ctx, uint32_t textCount, uint8_t styleMask, bool metadataOnly,
                        bool loadKernLig, bool accumulate) {
  if (!loaded_ || getter == nullptr) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Cap the unique-codepoint budget by what the heap can actually hold as a
  // full mini arena (glyph structs + bitmaps, working headroom left over).
  // Multi-string batches only: a several-hundred-chapter CJK table of
  // contents would otherwise extract up to MAX_PAGE_GLYPHS and fail the whole
  // arena allocation — better to load the first screens' worth and let
  // scrolling union-in the rest page by page. Per-string requests are small
  // and already bounded by the union gate in prewarmStyle (running the check
  // there would also log per draw call); metadata-only prewarms load no
  // bitmaps. Bytes/glyph prefers the measured average from the resident mini:
  // Hangul ink boxes run well under the advanceY-squared em estimate, which
  // otherwise roughly halves the usable budget.
  uint32_t cpBudget = MAX_PAGE_GLYPHS;
  if (!metadataOnly && textCount > 1) {
    uint8_t refStyle = MAX_STYLES;
    for (uint8_t si = 0; si < MAX_STYLES && refStyle == MAX_STYLES; si++) {
      if ((styleMask & (1 << si)) && styles_[si].present) refStyle = si;
    }
    if (refStyle < MAX_STYLES) {
      const auto& s = styles_[refStyle];
      const uint32_t bpp = s.header.is2Bit ? 2 : 1;
      uint32_t bitmapPerGlyph = (static_cast<uint32_t>(s.header.advanceY) * s.header.advanceY * bpp) / 8 + 4;
      if (s.miniGlyphCount > 0 && s.miniBitmapUsed > 0) {
        bitmapPerGlyph = s.miniBitmapUsed / s.miniGlyphCount;
      }
      const uint32_t perGlyph = bitmapPerGlyph + sizeof(EpdGlyph);
      constexpr uint32_t PREWARM_HEAP_HEADROOM = 16 * 1024;
      const uint32_t freeHeap = ESP.getFreeHeap();
      const uint32_t budgetBytes = freeHeap > PREWARM_HEAP_HEADROOM ? freeHeap - PREWARM_HEAP_HEADROOM : 0;
      const uint32_t budgetGlyphs = budgetBytes / (perGlyph > 0 ? perGlyph : 1);
      if (budgetGlyphs < cpBudget) {
        cpBudget = budgetGlyphs;
      }
    }
  }
  if (cpBudget == 0) return -1;

  // Step 1: Extract unique codepoints from the UTF-8 texts (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  for (uint32_t ti = 0; ti < textCount && cpCount < cpBudget; ti++) {
    const char* text = getter(ctx, ti);
    if (text == nullptr) continue;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p && cpCount < cpBudget) {
      uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;

      bool found = false;
      for (uint32_t i = 0; i < cpCount; i++) {
        if (codepoints[i] == cp) {
          found = true;
          break;
        }
      }
      if (!found) {
        codepoints[cpCount++] = cp;
      }
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly && loadKernLig) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    int missedForStyle = prewarmStyle(si, codepoints.get(), cpCount, metadataOnly, loadKernLig, accumulate);
    if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
      // The arena is one contiguous block, so a fragmented heap can fail it with
      // ample free bytes. Retry with the estimated largest prefix, backing off
      // if variable-size glyphs made that estimate too large.
      const uint32_t perGlyph = styles_[si].measuredBytesPerGlyph > 0 ? styles_[si].measuredBytesPerGlyph : 1;
      const uint32_t maxAlloc = ESP.getMaxAllocHeap();
      const uint32_t arenaBytes = maxAlloc > PREWARM_MAX_ALLOC_RESERVE ? maxAlloc - PREWARM_MAX_ALLOC_RESERVE : 0;
      uint32_t fit = arenaBytes / perGlyph;
      if (fit > cpCount) fit = cpCount;
      while (fit > 0) {
        LOG_DBG("SDCF", "Arena retry: %u -> %u glyphs (%uB/glyph, maxAlloc=%u)", cpCount, fit, perGlyph, maxAlloc);
        missedForStyle = prewarmStyle(si, codepoints.get(), fit, metadataOnly, loadKernLig, accumulate);
        if (missedForStyle != PREWARM_ARENA_TOO_LARGE) break;
        fit /= 2;
      }
      if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
        missedForStyle = static_cast<int>(cpCount);  // nothing resident
      } else {
        missedForStyle += static_cast<int>(cpCount - fit);  // the dropped suffix is absent too
      }
    }
    totalMissed += missedForStyle;
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                             bool loadKernLig, bool accumulate) {
  auto& s = styles_[styleIdx];

  // Idle-prewarm hit: mini data persists across PrewarmScopes (resetStyleMiniData
  // keeps it), so when the previous scope -- typically the idle prewarm of this
  // exact page -- already loaded every requested codepoint the font covers, this
  // page needs zero SD reads. A mini built metadata-only cannot serve a full
  // request (no bitmaps). Any uncovered codepoint falls through to the rebuild.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
        if (cp < s.miniIntervals[iv].first) break;  // intervals sorted ascending
        if (cp <= s.miniIntervals[iv].last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;  // not in font coverage: the rebuild couldn't load it either
      } else {
        covered = false;
      }
    }
    if (covered) {
      // A kern-wanting request (reader path) can subset-hit a mini that a
      // kern-free UI prewarm built: top up the kern matrix for the requested
      // codepoints without re-reading any glyphs.
      if (!metadataOnly && loadKernLig && s.miniKernLeftClassCount == 0 && s.header.kernLeftEntryCount > 0) {
        if (loadStyleKernLigatureData(s) && buildMiniKernMatrix(s, codepoints, cpCount)) {
          applyKernLigaturePointers(s, s.miniData);
        }
      }
      return missedInMini;
    }
  }

  // Trim oversized buffers only on a cache miss. Trimming when a scope closes
  // would discard a freshly prefetched page before its actual draw. Count each
  // rebuild once, retaining the arena until several pages use less than 3/4.
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        LOG_DBG("SDCF", "mini release (underuse): used=%u cap=%u", s.miniBitmapUsed, s.miniBitmapCapacity);
        freeStyleMiniData(s);
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }

  // Incremental callers merge the resident mini's codepoints so the rebuild below
  // accumulates instead of replacing. Screens draw several distinct fallback
  // strings per refresh (file browser rows, chapter lists, the reader status
  // bar after the page scope); replacing meant every string evicted every
  // other string's glyphs, so each measure/draw re-hit the SD forever. With
  // the union, residency converges after one pass and redraws are RAM-only.
  // Over MAX_PAGE_GLYPHS the union is abandoned (request-only rebuild), which
  // bounds mini RAM to the same worst case as a single dense page.
  std::unique_ptr<uint32_t[]> unionCps;
  if (accumulate && s.miniGlyphCount > 0 && s.miniIntervalCount > 0 && ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP) {
    // Heap-tight (e.g. a chapter list stacked over an open book). Size-aware:
    // a small union (a UI screen's worth of titles, a few KB) is exactly what
    // stops per-string eviction from re-reading the SD on every repaint, so
    // allow it as long as the estimated arena leaves headroom. Only when the
    // union would crowd the remaining heap (page-scale arenas) drop the
    // retained data and rebuild request-only below: bounded like the
    // pre-merge behavior, and the freed arena gives the small alloc room.
    const uint32_t unionMaxCount = s.miniGlyphCount + cpCount;  // pre-dedup upper bound
    const uint32_t avgBitmapBytes =
        (s.miniBitmapUsed > 0 && s.miniGlyphCount > 0) ? s.miniBitmapUsed / s.miniGlyphCount : 64;
    const uint32_t estArenaBytes = unionMaxCount * (static_cast<uint32_t>(sizeof(EpdGlyph)) + avgBitmapBytes);
    constexpr uint32_t UNION_PRESSURE_HEADROOM = 12 * 1024;
    if (estArenaBytes + UNION_PRESSURE_HEADROOM > ESP.getFreeHeap()) {
      freeStyleMiniData(s);
    }
  }
  if (accumulate && s.miniGlyphCount > 0 && s.miniIntervalCount > 0) {
    const uint32_t unionMax = s.miniGlyphCount + cpCount;
    unionCps.reset(new (std::nothrow) uint32_t[unionMax]);
    if (unionCps) {
      // Two-way sorted merge: resident stream walks the mini intervals
      // (ascending), request stream is the caller's sorted codepoint array.
      uint32_t n = 0;
      uint32_t ivIdx = 0;
      uint32_t ivCp = s.miniIntervals[0].first;
      bool ivActive = true;
      uint32_t ri = 0;
      while ((ivActive || ri < cpCount) && n < unionMax) {
        uint32_t next;
        if (ivActive && (ri >= cpCount || ivCp <= codepoints[ri])) {
          next = ivCp;
          if (ri < cpCount && codepoints[ri] == ivCp) ri++;
          if (ivCp < s.miniIntervals[ivIdx].last) {
            ivCp++;
          } else if (++ivIdx < s.miniIntervalCount) {
            ivCp = s.miniIntervals[ivIdx].first;
          } else {
            ivActive = false;
          }
        } else {
          next = codepoints[ri++];
        }
        unionCps[n++] = next;
      }
      if (!ivActive && ri >= cpCount && n <= MAX_PAGE_GLYPHS) {
        // A full mini must stay full: a metadata-only request may not drop
        // bitmaps other strings are still rendering from.
        metadataOnly = metadataOnly && s.miniMetadataOnly;
        codepoints = unionCps.get();
        cpCount = n;
      } else {
        unionCps.reset();
      }
    }
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  auto mappings = makeUniqueNoThrow<CpGlyphMapping[]>(cpCount);
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints. Reset counts and fall back to the
  // stub until the rebuild completes, but KEEP the existing buffers (keep-if-fits
  // reuse) — the free-and-realloc-per-page pattern here was a primary fragmenter.
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;

  if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Mini glyph array (reused across pages when it fits)
  if (!ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  s.miniGlyphCount = validCount;

  // Build sorted read order for sequential I/O
  auto readOrder = makeUniqueNoThrow<uint32_t[]>(validCount);
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder.get(), readOrder.get() + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        file.close();
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    lastReadIndex = gIdx;
  }

  // Mapping is no longer needed once glyph metadata has been read.
  mappings.reset();
  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    // Rounded up: the retry multiplies this back out to size a glyph set, and a
    // floored figure can yield an arena that still does not fit.
    if (validCount > 0) s.measuredBytesPerGlyph = (totalBitmapSize + validCount - 1) / validCount;

    // Release temporary allocations before replacing the large bitmap so their
    // holes can coalesce with the old arena. Recreate only the small read order.
    if (totalBitmapSize > s.miniBitmapCapacity) {
      readOrder.reset();
      psramDeleteArray(s.miniBitmap);
      s.miniBitmap = nullptr;
      s.miniBitmapCapacity = 0;
      if (ESP.getMaxAllocHeap() < totalBitmapSize) {
        // Growing metadata can split the old bitmap's free region. Reserve the
        // bitmap before rebuilding that metadata; the reserved capacity makes
        // this a single extra pass, with no recursive growth on the next pass.
        freeStyleMiniData(s);
        file = HalFile{};
        bool bitmapReady = ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize);
        if (!bitmapReady && hasAdvanceCache()) {
          // Layout advances are rebuildable; release them before falling back to fewer glyphs.
          const uint32_t largestBefore = ESP.getMaxAllocHeap();
          clearPersistentCache();
          LOG_DBG("SDCF", "Retrying bitmap after advance-cache release: maxAlloc=%u -> %u", largestBefore,
                  ESP.getMaxAllocHeap());
          bitmapReady = ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize);
        }
        if (!bitmapReady) {
          LOG_ERR("SDCF", "Failed to reserve mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
          return PREWARM_ARENA_TOO_LARGE;
        }
        LOG_DBG("SDCF", "Reserved bitmap before metadata rebuild (%u bytes)", totalBitmapSize);
        stats_.sdReadTimeMs += millis() - sdStart;
        stats_.seekCount += seekCount;
        return prewarmStyle(styleIdx, codepoints, cpCount, metadataOnly, loadKernLig, accumulate);
      }
    }
    if (!ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize)) {
      LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
      freeStyleMiniData(s);
      return PREWARM_ARENA_TOO_LARGE;
    }
    s.miniBitmapUsed = totalBitmapSize;  // underuse-hysteresis signal for the next cache miss

    if (!readOrder) {
      readOrder = makeUniqueNoThrow<uint32_t[]>(validCount);
      if (!readOrder) {
        LOG_ERR("SDCF", "Failed to allocate bitmap read order for style %u", styleIdx);
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
    }

    // Read bitmap data sorted by file offset
    std::sort(readOrder.get(), readOrder.get() + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          file.close();
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        seekCount++;
      }
      if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  readOrder.reset();

  // Full render prewarm: load the persistent kern classes + ligatures (one-time
  // per style, small — the big matrix is NOT loaded here) and then build the
  // per-page mini kern matrix restricted to class pairs reachable from this
  // page's codepoints. Skip during metadata-only prewarm — layout only needs
  // advanceX and the mini kern would be thrown away before rendering.
  bool kernLigOk = false;
  if (!metadataOnly && loadKernLig) {
    if (loadStyleKernLigatureData(s)) {
      kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount);
    }
  }

  // Populate miniData and swap
  s.miniMetadataOnly = metadataOnly;
  s.miniHysteresisPending = !metadataOnly;  // one hysteresis evaluation per rebuild
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.coverageHandler = &SdCardFont::onCoverageQuery;

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

// --- Advance table ---

namespace {
// Returns -1 on I/O failure, 0 for varying advances, 1 for a verified uniform interval.
int readUniformAdvance(HalFile& file, uint32_t offset, uint32_t count, uint16_t& advance) {
  EpdGlyph glyphs[8];
  if (!file.seekSet(offset)) return -1;
  for (uint32_t i = 0; i < count;) {
    const uint32_t batch = std::min<uint32_t>(8, count - i);
    if (file.read(reinterpret_cast<uint8_t*>(glyphs), batch * sizeof(EpdGlyph)) != batch * sizeof(EpdGlyph)) return -1;
    if (i == 0) advance = glyphs[0].advanceX;
    for (uint32_t j = 0; j < batch; ++j) {
      if (glyphs[j].advanceX != advance) return 0;
    }
    i += batch;
  }
  return 1;
}
}  // namespace

bool SdCardFont::prepareUniformAdvances() {
  if (uniformAdvancesScanned_) return true;
  static constexpr uint32_t MIN_UNIFORM_GLYPHS = 16;
  static constexpr uint8_t MAX_UNIFORM_INTERVALS = 16;
  // At most 256 temporary bytes, before layout. A heap buffer bounds the small
  // task stack; the font retains only the verified records in one exact-sized block.
  auto verified = makeUniqueNoThrow<UniformAdvance[]>(MAX_STYLES * MAX_UNIFORM_INTERVALS);
  if (!verified) {
    LOG_ERR("SDCF", "OOM: uniform advance scan");
    return false;
  }
  uint8_t ends[MAX_STYLES] = {};
  uint8_t count = 0;
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open font for uniform advances");
    return false;
  }
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    const auto& s = styles_[si];
    const uint8_t begin = count;
    if (s.present) {
      for (uint32_t i = 0; i < s.header.intervalCount && count - begin < MAX_UNIFORM_INTERVALS; ++i) {
        const uint32_t first = s.intervalsAreBmp16 ? s.bmpIntervals[i].first : s.fullIntervals[i].first;
        const uint32_t last = s.intervalsAreBmp16 ? s.bmpIntervals[i].last : s.fullIntervals[i].last;
        const uint32_t offset = s.intervalsAreBmp16 ? s.bmpIntervals[i].offset : s.fullIntervals[i].offset;
        const uint32_t length = last - first + 1;
        if (length < MIN_UNIFORM_GLYPHS) continue;
        uint16_t advance;
        const int result = readUniformAdvance(file, s.glyphsFileOffset + offset * sizeof(EpdGlyph), length, advance);
        if (result < 0) {
          LOG_ERR("SDCF", "Failed to verify advance interval %u style %u", i, si);
          return false;
        }
        if (result > 0) verified[count++] = {static_cast<uint16_t>(i), advance};
      }
    }
    ends[si] = count;
  }
  if (count) {
    uniformAdvances_ = psramNewArray<UniformAdvance>(count);
    if (!uniformAdvances_) {
      LOG_ERR("SDCF", "OOM: %u uniform advance records", count);
      return false;
    }
    std::copy_n(verified.get(), count, uniformAdvances_);
  }
  std::copy_n(ends, MAX_STYLES, uniformAdvanceEnd_);
  uniformAdvancesScanned_ = true;
  return true;
}

bool SdCardFont::prepareAdvances() {
  if (!loaded_) return false;
  const auto start = millis();
  bool complete = prepareUniformAdvances();
  if (!advanceTable_) {
    bool allStylesUniform = uniformAdvancesScanned_;
    for (uint8_t si = 0; si < MAX_STYLES; ++si) {
      const auto& s = styles_[si];
      if (!s.present) continue;
      uint32_t uniformGlyphs = 0;
      for (uint8_t i = si ? uniformAdvanceEnd_[si - 1] : 0; i < uniformAdvanceEnd_[si]; ++i) {
        const auto index = uniformAdvances_[i].intervalIndex;
        uniformGlyphs += s.intervalsAreBmp16 ? s.bmpIntervals[index].last - s.bmpIntervals[index].first + 1
                                             : s.fullIntervals[index].last - s.fullIntervals[index].first + 1;
      }
      if (uniformGlyphs * 4 < s.header.glyphCount * 3) allStylesUniform = false;
    }
    // Fonts with at least 75% uniform glyphs use a smaller shared pool. Others keep the
    // full per-style budget. One font-owned pool is fixed before layout begins.
    const uint16_t capacity = allStylesUniform
                                  ? std::max<uint16_t>(ADVANCE_STYLE_BUDGET, ADVANCE_STYLE_SHARE * styleCount_)
                                  : ADVANCE_STYLE_BUDGET * styleCount_;
    advanceTable_ = psramNewArray<AdvanceEntry>(capacity);
    if (!advanceTable_) {
      LOG_ERR("SDCF", "OOM: advance exceptions (%u bytes)", capacity * sizeof(AdvanceEntry));
      complete = false;
    } else {
      advanceTableCapacity_ = capacity;
    }
    LOG_DBG("SDCF", "Advance metrics: %u uniform intervals, exceptions=%u bytes", uniformAdvanceEnd_[MAX_STYLES - 1],
            advanceTableCapacity_ * sizeof(AdvanceEntry));
  }
  advancesPrepared_ = true;
  LOG_DBG("SDCF", "Advance preparation: %ums", millis() - start);
  return complete;
}

void SdCardFont::clearPersistentCache() {
  logAdvanceStats("release");
  psramDeleteArray(advanceTable_);
  advanceTable_ = nullptr;
  advanceTableCount_ = advanceTableCapacity_ = 0;
  std::fill_n(advanceTableSize_, MAX_STYLES, 0);
}

void SdCardFont::logAdvanceStats(const char* label) const {
#if LOG_LEVEL >= 2
  if (!loaded_ || !advancesPrepared_) return;
  LOG_DBG("SDCF", "Advance usage [%s] font=%08x pool=%u/%u %s", label, contentHash_, advanceTableCount_,
          advanceTableCapacity_, filePath_);
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    const auto& s = styles_[si];
    if (!s.present) continue;
    LOG_DBG("SDCF",
            "Advance usage font=%08x style=%u entries=%u peak=%u direct_reads=%u full_misses=%u reserved=%u uniform=%u "
            "scanned=%u",
            contentHash_, si, advanceTableSize_[si], advancePeak_[si], advanceDirectReads_[si], advanceFullMisses_[si],
            advanceTableCapacity_, uniformAdvanceEnd_[si] - (si ? uniformAdvanceEnd_[si - 1] : 0),
            uniformAdvancesScanned_);
  }
#else
  (void)label;
#endif
}

bool SdCardFont::uniformAdvanceLookup(uint8_t styleIdx, uint16_t intervalIndex, uint16_t* outAdvance) const {
  const uint8_t begin = styleIdx ? uniformAdvanceEnd_[styleIdx - 1] : 0;
  for (uint8_t i = begin; i < uniformAdvanceEnd_[styleIdx]; ++i) {
    if (uniformAdvances_[i].intervalIndex == intervalIndex) {
      if (outAdvance) *outAdvance = uniformAdvances_[i].advanceX;
      return true;
    }
  }
  return false;
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint16_t glyphIndex, uint16_t* outAdvance) const {
  const uint32_t key = (static_cast<uint32_t>(styleIdx) << 16) | glyphIndex;
  uint16_t lo = 0, hi = advanceTableCount_;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    if ((advanceTable_[mid] >> 14) < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < advanceTableCount_ && (advanceTable_[lo] >> 14) == key) {
    if (outAdvance) *outAdvance = advanceTable_[lo] & MAX_CACHED_ADVANCE;
    return true;
  }
  return false;
}

bool SdCardFont::canCacheAdvance(uint8_t styleIdx) const {
  return advanceTable_ &&
         (advanceTableCount_ < advanceTableCapacity_ || advanceTableSize_[styleIdx] < ADVANCE_STYLE_SHARE);
}

void SdCardFont::cacheAdvance(uint8_t styleIdx, uint16_t glyphIndex, uint16_t advance) {
  if (!canCacheAdvance(styleIdx) || advance > MAX_CACHED_ADVANCE) return;
  if (advanceTableCount_ == advanceTableCapacity_) {
    // A style may borrow spare slots, but cannot starve a later style's share.
    uint8_t donor = 0;
    for (uint8_t si = 1; si < MAX_STYLES; ++si) {
      if (advanceTableSize_[si] > advanceTableSize_[donor]) donor = si;
    }
    if (advanceTableSize_[donor] <= ADVANCE_STYLE_SHARE) return;
    uint16_t pos = advanceTableCount_;
    while (pos && (advanceTable_[pos - 1] >> 30) != donor) --pos;
    --pos;
    std::move(advanceTable_ + pos + 1, advanceTable_ + advanceTableCount_, advanceTable_ + pos);
    --advanceTableCount_;
    --advanceTableSize_[donor];
  }
  const uint32_t entry = (static_cast<uint32_t>(styleIdx) << 30) | (static_cast<uint32_t>(glyphIndex) << 14) | advance;
  uint16_t pos = advanceTableCount_;
  while (pos && advanceTable_[pos - 1] > entry) {
    advanceTable_[pos] = advanceTable_[pos - 1];
    --pos;
  }
  advanceTable_[pos] = entry;
  ++advanceTableCount_;
  ++advanceTableSize_[styleIdx];
#if LOG_LEVEL >= 2
  advancePeak_[styleIdx] = std::max(advancePeak_[styleIdx], advanceTableSize_[styleIdx]);
#endif
}

bool SdCardFont::hasAdvanceCache() const { return advanceTable_ != nullptr; }

bool SdCardFont::hasAdvanceTable() const { return advancesPrepared_; }

bool SdCardFont::readAdvance(HalFile& file, const PerStyle& s, uint16_t glyphIndex, uint16_t* outAdvance) const {
  EpdGlyph glyph;
  if (!file.seekSet(s.glyphsFileOffset + static_cast<uint32_t>(glyphIndex) * sizeof(EpdGlyph)) ||
      file.read(reinterpret_cast<uint8_t*>(&glyph), sizeof(glyph)) != sizeof(glyph)) {
    LOG_ERR("SDCF", "Failed to read advance for glyph %u", glyphIndex);
    return false;
  }
  *outAdvance = glyph.advanceX;
  return true;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  const auto& s = styles_[style];
  if (!advancesPrepared_ || !s.present) return 0;
  uint16_t interval;
  int32_t index = findGlobalGlyphIndex(s, codepoint, &interval);
  if (index < 0) index = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH, &interval);
  if (index < 0) return 0;
  uint16_t advance;
  if (uniformAdvanceLookup(style, interval, &advance) || advanceTableLookup(style, index, &advance)) return advance;
  // Cache pressure must not switch layout to bitmap/kerning measurement.
  // HalFile owns its short-lived file handle; no glyph bitmap or scratch array is needed.
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open font for advance lookup");
    return 0;
  }
#if LOG_LEVEL >= 2
  // Count metadata read attempts, including short reads; saturate long sessions.
  if (advanceDirectReads_[style] != UINT32_MAX) ++advanceDirectReads_[style];
  if (advanceTable_ && !canCacheAdvance(style) && advanceFullMisses_[style] != UINT32_MAX) {
    ++advanceFullMisses_[style];
  }
#endif
  return readAdvance(file, s, index, &advance) ? advance : 0;
}

void SdCardFont::fetchAdvances(uint8_t styleIdx, uint16_t* glyphIndices, uint8_t count) {
  if (count == 0) return;
  std::sort(glyphIndices, glyphIndices + count);
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open font for advance warmup");
    return;
  }
  for (uint8_t i = 0; i < count && canCacheAdvance(styleIdx); ++i) {
    uint16_t advance;
    if (!readAdvance(file, styles_[styleIdx], glyphIndices[i], &advance)) break;
    cacheAdvance(styleIdx, glyphIndices[i], advance);
  }
}

int SdCardFont::buildAdvanceTablePacked(const char* const* segments, const size_t* segmentLens,
                                        const size_t segmentCount, const bool includeSpace, const bool includeHyphen,
                                        uint8_t styleMask, const char* extraText) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  // Fonts first encountered during layout can use direct metrics without
  // allocating a long-lived table amid the parser's transient allocations.
  advancesPrepared_ = true;
  const auto start = millis();
  int missed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; ++si) {
    if (!(styleMask & (1 << si)) || !canCacheAdvance(si)) continue;
    const auto& s = styles_[si];
    uint16_t pending[64];
    uint8_t count = 0;
    const auto collect = [&](uint32_t cp) {
      if (!canCacheAdvance(si)) return;
      uint16_t interval;
      int32_t index = findGlobalGlyphIndex(s, cp, &interval);
      if (index < 0) index = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH, &interval);
      if (index < 0) {
        ++missed;
        return;
      }
      if (uniformAdvanceLookup(si, interval, nullptr) || advanceTableLookup(si, index, nullptr)) return;
      if (std::find(pending, pending + count, index) != pending + count) return;
      pending[count++] = static_cast<uint16_t>(index);
      if (count == 64) {
        fetchAdvances(si, pending, count);
        count = 0;
      }
    };
    for (size_t seg = 0; seg < segmentCount; ++seg) {
      const auto* p = reinterpret_cast<const uint8_t*>(segments[seg]);
      const auto* end = p + segmentLens[seg];
      while (p < end) {
        if (*p == 0)
          ++p;
        else
          collect(utf8NextCodepoint(&p));
      }
    }
    if (extraText) {
      const auto* p = reinterpret_cast<const uint8_t*>(extraText);
      while (*p) collect(utf8NextCodepoint(&p));
    }
    if (includeSpace) collect(' ');
    if (includeHyphen) collect('-');
    fetchAdvances(si, pending, count);
  }
  stats_.prewarmTotalMs = millis() - start;
  return missed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask, const char* extraText) {
  const size_t len = strlen(utf8Text);
  return buildAdvanceTablePacked(&utf8Text, &len, 1, false, false, styleMask, extraText);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = psramNewArray<uint8_t>(tempGlyph.dataLength);
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      psramDeleteArray(tempBitmap);
      file.close();
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      psramDeleteArray(tempBitmap);
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    psramDeleteArray(self->overflow_[slot].bitmap);
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
