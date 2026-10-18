#include "SdCardFont.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap() for the prewarm arena retry
#include <BuildArena.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

// FNV-1a hash for content-based font ID generation
static constexpr uint32_t FNV_OFFSET = 2166136261u;
static constexpr uint32_t FNV_PRIME = 16777619u;

static uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
static constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
static constexpr uint16_t CPFONT_VERSION = 4;
static constexpr uint32_t HEADER_SIZE = 32;
static constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;
// Working headroom left outside the mini bitmap arena's single contiguous block, for the
// allocations prewarmStyle still makes after it.
static constexpr uint32_t PREWARM_MAX_ALLOC_RESERVE = 4 * 1024;

// Helper to read little-endian values from byte buffer
static inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// --- Allocation (see useArena) ---

template <typename T>
T* SdCardFont::allocArray(const size_t count) {
  if (arena_) return arena_->allocArray<T>(count);
  return new (std::nothrow) T[count];
}

template <typename T>
void SdCardFont::freeArray(T* p) {
  if (!arena_) delete[] p;
}

template <typename T>
SdCardFont::Scratch<T> SdCardFont::makeScratch(const size_t count) {
  return Scratch<T>(allocArray<T>(count), ScratchDeleter<T>{arena_ == nullptr});
}

uint32_t SdCardFont::maxAllocatable() const {
  if (arena_) return static_cast<uint32_t>(arena_->capacity() - arena_->used());
  return ESP.getMaxAllocHeap();
}

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  freeArray(s.miniIntervals);
  s.miniIntervals = nullptr;
  freeArray(s.miniGlyphs);
  s.miniGlyphs = nullptr;
  freeArray(s.miniBitmap);
  s.miniBitmap = nullptr;
  s.miniBitmapBytes = 0;
  s.miniInkOnly = false;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniMode = PerStyle::MiniMode::NONE;
  // Clear dangling pointers in miniData and stubData (kern data points to freed mini arrays)
  s.miniData.kernLeftClasses = nullptr;
  s.miniData.kernRightClasses = nullptr;
  s.miniData.kernLeftCodepoints = nullptr;  // SD fonts use the packed class maps, not the split ones
  s.miniData.kernLeftClassIds = nullptr;
  s.miniData.kernRightCodepoints = nullptr;
  s.miniData.kernRightClassIds = nullptr;
  s.miniData.kernMatrix = nullptr;
  s.miniData.kernRowOffsets = nullptr;  // SD fonts are always dense; see EpdFontData::kernRowOffsets
  s.miniData.kernSparseCols = nullptr;
  s.miniData.kernSparseValues = nullptr;
  s.miniData.kernLeftEntryCount = 0;
  s.miniData.kernRightEntryCount = 0;
  s.miniData.kernLeftClassCount = 0;
  s.miniData.kernRightClassCount = 0;
  s.stubData.kernLeftClasses = nullptr;
  s.stubData.kernRightClasses = nullptr;
  s.stubData.kernLeftCodepoints = nullptr;  // SD fonts use the packed class maps, not the split ones
  s.stubData.kernLeftClassIds = nullptr;
  s.stubData.kernRightCodepoints = nullptr;
  s.stubData.kernRightClassIds = nullptr;
  s.stubData.kernMatrix = nullptr;
  s.stubData.kernRowOffsets = nullptr;
  s.stubData.kernSparseCols = nullptr;
  s.stubData.kernSparseValues = nullptr;
  s.stubData.kernLeftEntryCount = 0;
  s.stubData.kernRightEntryCount = 0;
  s.stubData.kernLeftClassCount = 0;
  s.stubData.kernRightClassCount = 0;
  s.onDemandMissCount = 0;
  s.onDemandMissLogged = 0;
  // NOTE: reportedMissCount is intentionally NOT reset here. The merge path
  // calls freeStyleMiniData() to swap mini buffers, and resetting the miss
  // tracker every paragraph would re-spam the log for the same 4 missing cps.
  // It IS reset by freeStyleAll() (font teardown) and clearAccumulation()
  // (section boundary), which are the right granularity for "forget what
  // we've reported".
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  if (metadataOwned_) {
    // Kern class tables: heap-owned only for pure SD fonts (mmapDataBase_ == nullptr).
    // For mmap fonts they alias flash (EpdKernClassEntry is fully packed — safe).
    if (!mmapDataBase_) {
      freeArray(s.kernLeftClasses);
      freeArray(s.kernRightClasses);
    }
    // Ligature pairs are always heap-owned when metadataOwned_ is true
    // (EpdLigaturePair contains uint32_t, copied to heap in loadFromMmap).
    freeArray(s.ligaturePairs);
  }
  s.kernLeftClasses = nullptr;
  s.kernClassesLoaded = false;
  s.kernRightClasses = nullptr;
  s.ligaturePairs = nullptr;
  s.ligLoaded = false;
  // Clear dangling pointers in EpdFontData structs
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  freeArray(s.miniKernLeftClasses);
  s.miniKernLeftClasses = nullptr;
  freeArray(s.miniKernRightClasses);
  s.miniKernRightClasses = nullptr;
  freeArray(s.miniKernMatrix);
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  s.reportedMissCount = 0;
  if (metadataOwned_ && s.intervalsOwner < 0) {
    freeArray(s.fullIntervals);
  }
  s.fullIntervals = nullptr;
  s.intervalsOwner = -1;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
  metadataOwned_ = true;
  mmapDataBase_ = nullptr;
}

void SdCardFont::unloadMetadata() {
  if (!loaded_) return;
  // For mmap-sourced fonts the heap copies can be dropped (they will be re-copied
  // from flash by reloadMetadata) — but since the mmap data is always accessible
  // and re-copying is cheap, keep the no-op behaviour: the heap copies stay live
  // and there is nothing to reload. Phase lifecycle overhead is zero.
  if (mmapDataBase_) {
    LOG_DBG("SDCF", "unloadMetadata: mmap font — no-op (%s)", filePath_);
    return;
  }
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;
    // Free fullIntervals and kern/lig tables — keeps file offsets and header counts intact.
    // A borrowed table is the owner's to free; the borrower keeps intervalsOwner for the reload.
    if (s.intervalsOwner < 0) {
      freeArray(s.fullIntervals);
    }
    s.fullIntervals = nullptr;
    freeStyleKernLigatureData(s);
    // Mini data is NOT freed — it is managed per-section and is already empty at the
    // chapter boundary where this is called.
  }
  LOG_DBG("SDCF", "Metadata unloaded: %s", filePath_);
}

bool SdCardFont::reloadMetadata() {
  if (!loaded_) return false;
  if (mmapDataBase_) {
    LOG_DBG("SDCF", "reloadMetadata: mmap font — no-op (%s)", filePath_);
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "reloadMetadata: failed to open %s", filePath_);
    return false;
  }

  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // A borrower re-points at its owner, which has a lower index and so was reloaded first.
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
      continue;
    }

    // Re-read fullIntervals using stored file offset
    if (!s.fullIntervals) {
      s.fullIntervals = allocArray<EpdUnicodeInterval>(s.header.intervalCount);
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "reloadMetadata: failed to alloc intervals for style %u", i);
        file.close();
        return false;
      }
      if (!file.seekSet(s.intervalsFileOffset)) {
        LOG_ERR("SDCF", "reloadMetadata: failed to seek intervals for style %u", i);
        file.close();
        return false;
      }
      const size_t sz = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), sz) != static_cast<int>(sz)) {
        LOG_ERR("SDCF", "reloadMetadata: failed to read intervals for style %u", i);
        file.close();
        return false;
      }
    }
  }

  file.close();

  // Kern/lig tables are lazy-loaded on the next prewarm call — no need to reload eagerly.
  // The existing loadStyleKernLigatureData() path handles this transparently.
  LOG_DBG("SDCF", "Metadata reloaded: %s", filePath_);
  return true;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    delete[] overflow_[i].bitmap;  // heap even in arena mode (OverflowEntry::bitmap)
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
    overflow_[i].styleIdx = 0;
    overflow_[i].occupied = false;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(const PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  // Packed form, as stored in the .cpfont; the split arrays are built-in only.
  data.kernLeftCodepoints = nullptr;
  data.kernLeftClassIds = nullptr;
  data.kernRightCodepoints = nullptr;
  data.kernRightClassIds = nullptr;
  data.kernMatrix = s.miniKernMatrix;
  // The .cpfont format stores a dense matrix and is mapped in place, so SD fonts never use the
  // sparse form the built-in fonts switched to. Set explicitly rather than relying on the
  // caller's initialisation: getKerning() picks the representation by which pointer is non-null.
  data.kernRowOffsets = nullptr;
  data.kernSparseCols = nullptr;
  data.kernSparseValues = nullptr;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s, bool ligatureOnly) {
  // During metadata-only (layout) prewarms, skip the kern class tables: the kern
  // matrix is never built at layout time so getKerning() returns 0 regardless.
  // Skipping them saves ~4KB per style (~17KB total for 4 styles), preventing OOM
  // on low-heap devices when long paragraphs try to grow their word vector.
  const bool wantKern = !ligatureOnly && s.header.kernLeftEntryCount > 0;
  const bool wantLig = s.header.ligaturePairCount > 0;

  const bool kernDone = !wantKern || s.kernClassesLoaded;
  const bool ligDone = !wantLig || s.ligLoaded;
  if (kernDone && ligDone) return true;

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (wantKern && !s.kernClassesLoaded) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    EpdKernClassEntry* newLeft = allocArray<EpdKernClassEntry>(s.header.kernLeftEntryCount);
    EpdKernClassEntry* newRight = allocArray<EpdKernClassEntry>(s.header.kernRightEntryCount);

    if (!newLeft || !newRight) {
      freeArray(newLeft);
      freeArray(newRight);
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      file.close();
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      freeArray(newLeft);
      freeArray(newRight);
      LOG_ERR("SDCF", "Failed to seek to kern data");
      file.close();
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(newLeft), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(newRight), rightSz) != static_cast<int>(rightSz)) {
      freeArray(newLeft);
      freeArray(newRight);
      LOG_ERR("SDCF", "Failed to read kern classes");
      file.close();
      return false;
    }
    s.kernLeftClasses = newLeft;
    s.kernRightClasses = newRight;
    s.kernClassesLoaded = true;
  }

  if (wantLig && !s.ligLoaded) {
    EpdLigaturePair* newLig = allocArray<EpdLigaturePair>(s.header.ligaturePairCount);
    if (!newLig) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      file.close();
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      freeArray(newLig);
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      file.close();
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(newLig), sz) != static_cast<int>(sz)) {
      freeArray(newLig);
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      file.close();
      return false;
    }
    s.ligaturePairs = newLig;
    s.ligLoaded = true;

    // Make ligatures visible to the stub (used when no mini data built yet).
    // Kern stays nullptr on the stub — it is only wired in miniData via
    // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
    s.stubData.ligaturePairs = s.ligaturePairs;
    s.stubData.ligaturePairCount = s.header.ligaturePairCount;
  }

  file.close();
  LOG_DBG("SDCF", "Kern/lig loaded: kernL=%u kernR=%u ligs=%u ligOnly=%d",
          s.kernClassesLoaded ? s.header.kernLeftEntryCount : 0u,
          s.kernClassesLoaded ? s.header.kernRightEntryCount : 0u, s.ligLoaded ? s.header.ligaturePairCount : 0u,
          ligatureOnly);
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
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount, HalFile& file) {
  freeStyleMiniKern(s);
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    return true;  // font has no kern classes — nothing to build
  }

  // 4× 256-byte scratch arrays: heap-allocated as one block to avoid blowing
  // the activity task's 8 KB stack when this runs deep in the parser → layout
  // → prewarm call chain (especially when invoked 4× per paragraph, once per
  // style).
  // Layout: [usedLeft 256][usedRight 256][leftRenumber 256][rightRenumber 256]
  //         [newToOldLeft 256][newToOldRight 256] = 1536 bytes total
  auto scratch = makeScratch<uint8_t>(6 * 256);
  if (scratch) memset(scratch.get(), 0, 6 * 256);
  if (!scratch) {
    LOG_ERR("SDCF", "Failed to allocate kern scratch (1536 bytes)");
    return false;
  }
  uint8_t* const base = scratch.get();
  uint8_t* usedLeft = base + 0 * 256;
  uint8_t* usedRight = base + 1 * 256;
  uint8_t* leftRenumber = base + 2 * 256;
  uint8_t* rightRenumber = base + 3 * 256;
  uint8_t* newToOldLeft = base + 4 * 256;
  uint8_t* newToOldRight = base + 5 * 256;

  // Class IDs are uint8_t, 0 meaning none. A font is a user-supplied file, and an ID
  // past the class count its header declares would index outside a matrix row and
  // point the SD sweep below past the end of the matrix -- where `matrixEnd -
  // chunkStart` underflows and the read asks for ~4 GB into a 4 KB buffer. Such an
  // entry counts as unkerned. Steps 1, 3 and 5 all go through these two lookups, so
  // they agree on which codepoints carry a class.
  // Ported from crosspoint-reader PR #3838 ("fix: keep SD-font kerning and ligatures
  // in edge cases", Sung-jin Brian Hong / @serialx), its bugs 2 and 3. Upstream drops
  // such entries when the class tables load; here those tables can point straight
  // into the read-only flash mapping, so the range check sits on the lookup instead.
  const auto leftClass = [&s](const uint32_t cp) -> uint8_t {
    const uint8_t c = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    return c <= s.header.kernLeftClassCount ? c : 0;
  };
  const auto rightClass = [&s](const uint32_t cp) -> uint8_t {
    const uint8_t c = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    return c <= s.header.kernRightClassCount ? c : 0;
  };

  // Step 1: mark used left/right classes.
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint8_t lc = leftClass(codepoints[i]);
    if (lc) usedLeft[lc] = 1;
    const uint8_t rc = rightClass(codepoints[i]);
    if (rc) usedRight[rc] = 1;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step. The counts are
  // uint16_t: a page can use all 255 classes, and a uint8_t count would make every
  // `<= num` loop below run forever, writing before the row on the way.
  uint16_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = static_cast<uint8_t>(numLeft);
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = static_cast<uint8_t>(numRight);
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (leftClass(codepoints[i]) != 0) miniLeftCount++;
    if (rightClass(codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: allocate the three mini buffers. The matrix is <1KB in practice
  // (<30 × <30 × 1 byte) so fragmentation is a non-issue.
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  s.miniKernLeftClasses = allocArray<EpdKernClassEntry>(miniLeftCount);
  s.miniKernRightClasses = allocArray<EpdKernClassEntry>(miniRightCount);
  s.miniKernMatrix = allocArray<int8_t>(matrixBytes);
  if (!s.miniKernLeftClasses || !s.miniKernRightClasses || !s.miniKernMatrix) {
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
    const uint8_t lc = leftClass(cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    const uint8_t rc = rightClass(cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: populate the mini matrix rows.
  //
  // Mmap path: the full kern matrix is directly pointer-accessible in flash.
  // Read each needed row via pointer arithmetic — no heap buffer, no seeks.
  //
  // SD path: read the matrix in fixed-size 4 KB chunks sweeping forward through
  // the file. This bounds the heap spike at KERN_CHUNK_BYTES instead of the full
  // matrix size (~28 KB for Literata). Falls back to per-row reads if the chunk
  // buffer allocation fails.
  const uint32_t rowBytes = s.header.kernRightClassCount;

  // Build a sorted-by-oldL mapping so we sweep the source forward.
  // Reuse the usedLeft/usedRight scratch slots — both are fully consumed after
  // step 1 and not read again. newToOldLeft/Right (slots 4 & 5) must stay intact.
  uint8_t* sortedOldL = base + 0 * 256;
  uint8_t* sortedNewL = base + 1 * 256;
  {
    uint8_t k = 0;
    for (int oldL = 1; oldL < 256; oldL++) {
      for (uint16_t newL = 1; newL <= numLeft; newL++) {
        if (newToOldLeft[newL] == static_cast<uint8_t>(oldL)) {
          sortedOldL[k] = static_cast<uint8_t>(oldL);
          sortedNewL[k] = static_cast<uint8_t>(newL);
          k++;
          break;
        }
      }
    }
  }

  if (mmapDataBase_) {
    // Mmap fast-path: kern matrix is at a fixed offset in the flash mapping.
    // Each row is a contiguous array of kernRightClassCount int8_t values.
    // Direct pointer read — no heap allocation, no SD I/O.
    const int8_t* matrixBase = reinterpret_cast<const int8_t*>(mmapDataBase_ + s.kernMatrixFileOffset);
    for (uint8_t k = 0; k < numLeft; k++) {
      const uint8_t oldL = sortedOldL[k];
      const uint8_t newL = sortedNewL[k];
      const int8_t* srcRow = matrixBase + (oldL - 1u) * rowBytes;
      int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
      for (uint16_t newR = 1; newR <= numRight; newR++) {
        miniRow[newR - 1] = srcRow[newToOldRight[newR] - 1u];
      }
    }
    LOG_DBG("SDCF", "Built mini kern (mmap): %u×%u=%u bytes", numLeft, numRight, matrixBytes);
  } else {
    // SD path: chunked sweep.
    static constexpr uint32_t KERN_CHUNK_BYTES = 4096;
    auto chunkBuf = makeScratch<int8_t>(KERN_CHUNK_BYTES);
    if (chunkBuf) {
      uint32_t chunkStart = UINT32_MAX;
      uint32_t chunkEnd = 0;
      uint32_t chunkSeeks = 0;
      for (uint8_t k = 0; k < numLeft; k++) {
        const uint8_t oldL = sortedOldL[k];
        const uint8_t newL = sortedNewL[k];
        const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * rowBytes;
        const uint32_t rowFileEnd = rowFileOff + rowBytes;
        if (rowFileOff < chunkStart || rowFileEnd > chunkEnd) {
          chunkStart = rowFileOff;
          uint32_t toRead = KERN_CHUNK_BYTES;
          const uint32_t matrixEnd =
              s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * rowBytes;
          if (chunkStart + toRead > matrixEnd) toRead = matrixEnd - chunkStart;
          if (!file.seekSet(chunkStart)) {
            LOG_ERR("SDCF", "Failed to seek to kern chunk at %u", chunkStart);
            freeStyleMiniKern(s);
            return false;
          }
          if (file.read(reinterpret_cast<uint8_t*>(chunkBuf.get()), toRead) != static_cast<int>(toRead)) {
            LOG_ERR("SDCF", "Failed to read kern chunk (%u bytes)", toRead);
            freeStyleMiniKern(s);
            return false;
          }
          chunkEnd = chunkStart + toRead;
          chunkSeeks++;
        }
        const int8_t* srcRow = chunkBuf.get() + (rowFileOff - chunkStart);
        int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
        for (uint16_t newR = 1; newR <= numRight; newR++) {
          miniRow[newR - 1] = srcRow[newToOldRight[newR] - 1u];
        }
      }
      LOG_DBG("SDCF", "Built mini kern (chunked %uB): %u×%u=%u bytes, %u seeks", KERN_CHUNK_BYTES, numLeft, numRight,
              matrixBytes, chunkSeeks);
    } else {
      // Fallback: one seek + one row read per used left class.
      LOG_DBG("SDCF", "Built mini kern (per-row fallback): %u rows", numLeft);
      auto rowBuf = makeScratch<int8_t>(rowBytes);
      if (!rowBuf) {
        LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", rowBytes);
        freeStyleMiniKern(s);
        return false;
      }
      for (uint8_t k = 0; k < numLeft; k++) {
        const uint8_t oldL = sortedOldL[k];
        const uint8_t newL = sortedNewL[k];
        const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * rowBytes;
        if (!file.seekSet(rowFileOff)) {
          LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
          freeStyleMiniKern(s);
          return false;
        }
        if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), rowBytes) != static_cast<int>(rowBytes)) {
          LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
          freeStyleMiniKern(s);
          return false;
        }
        int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
        for (uint16_t newR = 1; newR <= numRight; newR++) {
          miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
        }
      }
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = static_cast<uint8_t>(numLeft);
  s.miniKernRightClassCount = static_cast<uint8_t>(numRight);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  styleProviders_[styleIdx].self = this;
  styleProviders_[styleIdx].styleIdx = styleIdx;
  styles_[styleIdx].stubData.provider = &styleProviders_[styleIdx];
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

// --- Interval table sharing ---

// Returns the lowest earlier style that owns a table byte-identical to style `styleIdx`'s
// on-file intervals, or -1. Only owners are candidates, so a borrower never chains.
// Reads the file in stack-sized chunks, so a mismatch costs a re-read, never an allocation.
int8_t SdCardFont::findIdenticalIntervals(const uint8_t styleIdx, HalFile& file) {
  const auto& s = styles_[styleIdx];
  static constexpr uint32_t CHUNK = 16;  // 192 bytes of stack
  EpdUnicodeInterval chunk[CHUNK];
  for (uint8_t k = 0; k < styleIdx; k++) {
    const auto& owner = styles_[k];
    if (!owner.present || owner.intervalsOwner >= 0 || !owner.fullIntervals) continue;
    if (owner.header.intervalCount != s.header.intervalCount) continue;
    if (!file.seekSet(s.intervalsFileOffset)) return -1;
    bool same = true;
    for (uint32_t done = 0; same && done < s.header.intervalCount; done += CHUNK) {
      const uint32_t n = std::min(CHUNK, s.header.intervalCount - done);
      const int bytes = static_cast<int>(n * sizeof(EpdUnicodeInterval));
      same = file.read(reinterpret_cast<uint8_t*>(chunk), bytes) == bytes &&
             memcmp(chunk, owner.fullIntervals + done, bytes) == 0;
    }
    if (same) {
      LOG_DBG("SDCF", "Style %u shares style %u's %u-interval table (%u B not allocated)", styleIdx, k,
              s.header.intervalCount, static_cast<unsigned>(s.header.intervalCount * sizeof(EpdUnicodeInterval)));
      return static_cast<int8_t>(k);
    }
  }
  return -1;
}

int8_t SdCardFont::findIdenticalIntervals(const uint8_t styleIdx, const uint8_t* records) {
  const auto& s = styles_[styleIdx];
  const size_t bytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
  for (uint8_t k = 0; k < styleIdx; k++) {
    const auto& owner = styles_[k];
    if (!owner.present || owner.intervalsOwner >= 0 || !owner.fullIntervals) continue;
    if (owner.header.intervalCount != s.header.intervalCount) continue;
    if (memcmp(records, owner.fullIntervals, bytes) == 0) {
      LOG_DBG("SDCF", "Style %u shares style %u's %u-interval table (%u B not allocated)", styleIdx, k,
              s.header.intervalCount, static_cast<unsigned>(bytes));
      return static_cast<int8_t>(k);
    }
  }
  return -1;
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

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    file.close();
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    file.close();
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    file.close();
    return false;
  }

  // Begin content hash: accumulate global header.
  // KNOWN LIMITATION: hash covers global header + per-style TOC only, not the
  // payload sections (intervals / glyph metrics / kern / ligature / bitmap). A
  // font edit that alters payload bytes without changing any TOC count would
  // produce the same contentHash and could leave stale EPUB section caches.
  // Acceptable in practice because generate-sd-fonts.sh regeneration almost
  // always changes interval/glyph/kern counts; revisit if we see real-world
  // mismatches.
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  // Local name `numStyles` instead of `styleCount` to avoid shadowing the
  // member function styleCount() (cppcheck shadowFunction warning).
  uint8_t numStyles = headerBuf[12];
  if (numStyles == 0 || numStyles > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", numStyles);
    file.close();
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < numStyles; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      file.close();
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      continue;
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

    // Sanity-check counts to reject malformed files before allocating
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (intervals=%u, glyphs=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount);
      s.present = false;
      continue;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = numStyles;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // Compared against the file BEFORE allocating: de-duplicating after the read would still
    // need both copies resident at once, and that peak is what fails on a tight heap.
    s.intervalsOwner = findIdenticalIntervals(i, file);
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
    } else {
      s.fullIntervals = allocArray<EpdUnicodeInterval>(s.header.intervalCount);
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        file.close();
        freeAll();
        return false;
      }

      if (!file.seekSet(s.intervalsFileOffset)) {
        LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
        file.close();
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        file.close();
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

  file.close();
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

// --- Load from mmap ---

// Helper: inline read of a little-endian uint16_t / int16_t / uint32_t from a byte pointer
// (same helpers as the file-based loader above)
static inline uint16_t mmapU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int16_t mmapI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t mmapU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool SdCardFont::loadFromMmap(const uint8_t* base, size_t size, const char* sdPath) {
  freeAll();

  if (!base || size < HEADER_SIZE) {
    LOG_ERR("SDCF", "loadFromMmap: invalid pointer or size %u", static_cast<unsigned>(size));
    return false;
  }

  // Validate magic and version
  if (memcmp(base, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "loadFromMmap: invalid magic bytes");
    return false;
  }
  const uint16_t fileVersion = mmapU16(base + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "loadFromMmap: unsupported version %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Content hash: accumulate global header
  uint32_t hash = fnv1a(base, HEADER_SIZE);

  const bool is2Bit = (mmapU16(base + 10) & 1) != 0;
  const uint8_t numStyles = base[12];
  if (numStyles == 0 || numStyles > MAX_STYLES) {
    LOG_ERR("SDCF", "loadFromMmap: invalid style count %u", numStyles);
    return false;
  }

  // Parse style TOC
  for (uint8_t i = 0; i < numStyles; i++) {
    const uint8_t* tocEntry = base + HEADER_SIZE + i * STYLE_TOC_ENTRY_SIZE;
    if (tocEntry + STYLE_TOC_ENTRY_SIZE > base + size) {
      LOG_ERR("SDCF", "loadFromMmap: TOC entry %u out of bounds", i);
      freeAll();
      return false;
    }
    hash = fnv1a(tocEntry, STYLE_TOC_ENTRY_SIZE, hash);

    const uint8_t styleId = tocEntry[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "loadFromMmap: invalid styleId %u in TOC", styleId);
      continue;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = mmapU32(tocEntry + 4);
    s.header.glyphCount = mmapU32(tocEntry + 8);
    s.header.advanceY = tocEntry[12];
    s.header.ascender = mmapI16(tocEntry + 13);
    s.header.descender = mmapI16(tocEntry + 15);
    s.header.kernLeftEntryCount = mmapU16(tocEntry + 17);
    s.header.kernRightEntryCount = mmapU16(tocEntry + 19);
    s.header.kernLeftClassCount = tocEntry[21];
    s.header.kernRightClassCount = tocEntry[22];
    s.header.ligaturePairCount = tocEntry[23];
    s.header.is2Bit = is2Bit;

    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS) {
      LOG_ERR("SDCF", "loadFromMmap: style %u unreasonable counts", styleId);
      s.present = false;
      continue;
    }

    const uint32_t dataOffset = mmapU32(tocEntry + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = numStyles;
  contentHash_ = hash;

  // Copy metadata from the mmap region into heap-allocated, naturally-aligned
  // buffers. A direct reinterpret_cast into flash would be faster but is unsafe
  // on ESP32-C3 (RISC-V): section offsets are not guaranteed 4-byte aligned for
  // styles ≥1 (kern-class entries are 3 bytes each, kern matrix is byte-granularity),
  // so casting to EpdUnicodeInterval* / EpdLigaturePair* (both contain uint32_t
  // fields) would cause LoadAccessFault. We copy once at load time; subsequent
  // accesses are safe via naturally-aligned heap pointers.
  // metadataOwned_ = true so freeStyleAll() will delete[] these arrays.
  // mmapDataBase_ is kept for the kern MATRIX (buildMiniKernMatrix reads it as
  // raw bytes — no alignment requirement) and for glyph metrics (glyphsFileOffset
  // follows intervalCount*12 bytes which keeps 4-byte alignment for style 0; for
  // styles ≥1 the glyph section alignment is also guaranteed because EpdGlyph is
  // 16 bytes and intervals are 12 bytes — both multiples of 4 — so the combined
  // section size is always 4-byte aligned).
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // Copy fullIntervals into a heap buffer (alignment-safe on RISC-V).
    const size_t intervalsSz = s.header.intervalCount * sizeof(EpdUnicodeInterval);
    if (s.intervalsFileOffset + intervalsSz > size) {
      LOG_ERR("SDCF", "loadFromMmap: intervals for style %u out of bounds", i);
      freeAll();
      return false;
    }
    s.intervalsOwner = findIdenticalIntervals(i, base + s.intervalsFileOffset);
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
    } else {
      s.fullIntervals = allocArray<EpdUnicodeInterval>(s.header.intervalCount);
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "loadFromMmap: OOM for intervals style %u", i);
        freeAll();
        return false;
      }
      memcpy(s.fullIntervals, base + s.intervalsFileOffset, intervalsSz);
    }

    // Kern class tables: EpdKernClassEntry is __attribute__((packed)) with only
    // uint16_t + uint8_t members. GCC emits byte-load sequences for all member
    // accesses on packed structs, so aliasing flash directly is safe on RISC-V.
    // No heap copy needed — saves ~6 KB for a typical 4-style Latin font.
    if (s.header.kernLeftEntryCount > 0) {
      const size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
      const size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
      if (s.kernLeftFileOffset + leftSz + rightSz > size) {
        LOG_ERR("SDCF", "loadFromMmap: kern tables for style %u out of bounds", i);
        freeAll();
        return false;
      }
      s.kernLeftClasses = reinterpret_cast<EpdKernClassEntry*>(const_cast<uint8_t*>(base + s.kernLeftFileOffset));
      s.kernRightClasses = reinterpret_cast<EpdKernClassEntry*>(const_cast<uint8_t*>(base + s.kernRightFileOffset));
      s.kernClassesLoaded = true;
    }

    // Copy ligature pairs (EpdLigaturePair contains uint32_t — needs heap alignment).
    if (s.header.ligaturePairCount > 0) {
      const size_t ligSz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
      if (s.ligatureFileOffset + ligSz > size) {
        LOG_ERR("SDCF", "loadFromMmap: ligature table for style %u out of bounds", i);
        freeAll();
        return false;
      }
      s.ligaturePairs = allocArray<EpdLigaturePair>(s.header.ligaturePairCount);
      if (!s.ligaturePairs) {
        LOG_ERR("SDCF", "loadFromMmap: OOM for ligature pairs style %u", i);
        freeAll();
        return false;
      }
      memcpy(s.ligaturePairs, base + s.ligatureFileOffset, ligSz);
      s.ligLoaded = true;
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;
    s.stubData.ligaturePairs = s.ligaturePairs;
    s.stubData.ligaturePairCount = s.header.ligaturePairCount;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  // Store the SD path so the bitmap overflow handler can open the .cpfont from SD
  // for glyph bitmap data at draw time (metadata comes from flash, bitmaps from SD).
  if (sdPath && strlen(sdPath) < sizeof(filePath_)) {
    strncpy(filePath_, sdPath, sizeof(filePath_) - 1);
    filePath_[sizeof(filePath_) - 1] = '\0';
  }
  // Metadata was copied into heap arrays above — owned, must be delete[]'d on free.
  // mmapDataBase_ is kept for the kern MATRIX and glyph array (byte-safe mmap reads).
  metadataOwned_ = true;
  mmapDataBase_ = base;
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded from mmap: %u styles, size=%u", styleCount_, static_cast<unsigned>(size));
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d", i, h.intervalCount, h.glyphCount,
            h.advanceY, h.ascender, h.descender);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto& interval = s.fullIntervals[mid];
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval.offset + (codepoint - interval.first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, bool loadKernLigatureData) {
  if (!loaded_) return -1;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  auto codepoints = makeScratch<uint32_t>(MAX_PAGE_GLYPHS);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
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
  // Load ligature metadata when either doing a full prewarm or when
  // metadata-only layout measurement needs applyLigatures()/getKerning().
  if (!metadataOnly || loadKernLigatureData) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s, /*ligatureOnly=*/true);
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
  uint32_t* const cpBase = codepoints.get();
  std::sort(cpBase, cpBase + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    int missedForStyle = prewarmStyle(si, cpBase, cpCount, metadataOnly, loadKernLigatureData);

    // The bitmap arena is a single contiguous block, so on a fragmented heap it can fail with
    // plenty of free bytes -- and the whole style was being dropped for it, which costs every
    // glyph on the page rather than the few that would not fit. Retry with the largest prefix
    // the biggest free block can hold, halving if variable-width glyphs made that estimate
    // optimistic. Ported from crosspoint-reader PR #3126 (Sung-jin Brian Hong
    // <serialx@serialx.net>).
    if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
      const uint32_t perGlyph = styles_[si].measuredBytesPerGlyph > 0 ? styles_[si].measuredBytesPerGlyph : 1;
      // The arena's room when there is one: sized from the heap, every failed attempt would leave
      // its tables in the arena and the retry would never fit.
      const uint32_t maxAlloc = maxAllocatable();
      // Leave working headroom outside the arena: the read order and mappings are still
      // allocated after it inside prewarmStyle.
      const uint32_t arenaBytes = maxAlloc > PREWARM_MAX_ALLOC_RESERVE ? maxAlloc - PREWARM_MAX_ALLOC_RESERVE : 0;
      uint32_t fit = arenaBytes / perGlyph;
      if (fit > cpCount) fit = cpCount;
      while (fit > 0) {
        LOG_DBG("SDCF", "Arena retry: %u -> %u glyphs (%uB/glyph, maxAlloc=%u)", cpCount, fit, perGlyph, maxAlloc);
        missedForStyle = prewarmStyle(si, cpBase, fit, metadataOnly, loadKernLigatureData);
        if (missedForStyle != PREWARM_ARENA_TOO_LARGE) break;
        fit /= 2;
      }
      if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
        missedForStyle = static_cast<int>(cpCount);  // nothing resident, same as before
      } else {
        missedForStyle += static_cast<int>(cpCount - fit);  // the dropped suffix is missing too
      }
    }
    totalMissed += missedForStyle;
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

// Returns true iff every codepoint in `codepoints[0..cpCount)` is already covered
// by the style's current miniIntervals. O(cpCount * log intervalCount).
bool SdCardFont::allCpsCovered(const PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  if (s.miniIntervalCount == 0) return false;
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];
    // Binary search miniIntervals for the range containing cp.
    int lo = 0, hi = static_cast<int>(s.miniIntervalCount) - 1;
    bool found = false;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      const auto& iv = s.miniIntervals[mid];
      if (cp < iv.first) {
        hi = mid - 1;
      } else if (cp > iv.last) {
        lo = mid + 1;
      } else {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

// Packs a run of 2-bit glyph bytes (4 pixels each, first pixel in the top bits) into the glyph's ink
// mask (8 pixels a byte, first pixel in the top bit): a pixel is ink when its level is not 0, the
// rule GfxRenderer's black-and-white pass draws by (draw mask 0x0E, raw {1,2,3}). Writes
// (inBytes + 1) / 2 bytes. `out` may be `in`: byte j is written after bytes 2j and 2j + 1 are read.
static void packInkMask(const uint8_t* in, const uint32_t inBytes, uint8_t* out) {
  const auto nibble = [](const uint8_t b) -> uint8_t {
    return static_cast<uint8_t>(((b & 0xC0) ? 8 : 0) | ((b & 0x30) ? 4 : 0) | ((b & 0x0C) ? 2 : 0) |
                                ((b & 0x03) ? 1 : 0));
  };
  for (uint32_t j = 0; 2 * j < inBytes; j++) {
    const uint8_t hi = nibble(in[2 * j]);
    const uint8_t lo = 2 * j + 1 < inBytes ? nibble(in[2 * j + 1]) : 0;
    out[j] = static_cast<uint8_t>((hi << 4) | lo);
  }
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                             bool loadKernLigatureData) {
  auto& s = styles_[styleIdx];
  // Ink masks for a black-and-white-only draw (setInkOnlyPrewarm). Mmap bitmaps need no arena to
  // shrink; a 1-bit font is its own ink mask.
  const bool ink = inkOnlyPrewarm_ && !metadataOnly && s.header.is2Bit && !mmapDataBase_;

  // ---- Mmap metadata fast-path ----
  // For mmap fonts, fullIntervals and the glyph array are directly pointer-
  // accessible in flash. A metadata-only prewarm needs no heap copy and no SD
  // reads — wire miniData to the flash-resident tables once and return. The full
  // interval table covers every codepoint in the font, so allCpsCovered is
  // trivially always true for metadata queries.
  //
  // Full (bitmap) prewarms still go through the normal path: bitmaps are not in
  // the mmap region and must be read from SD.
  if (mmapDataBase_ && metadataOnly) {
    if (s.miniMode == PerStyle::MiniMode::NONE) {
      // First call: wire miniData directly to flash-resident data.
      // miniGlyphs/miniIntervals stay null — miniData.glyph and .intervals
      // point into flash and must not be freed via freeStyleMiniData.
      memset(&s.miniData, 0, sizeof(s.miniData));
      s.miniData.intervals = s.fullIntervals;
      s.miniData.intervalCount = s.header.intervalCount;
      s.miniData.glyph = reinterpret_cast<const EpdGlyph*>(mmapDataBase_ + s.glyphsFileOffset);
      s.miniData.advanceY = s.header.advanceY;
      s.miniData.ascender = s.header.ascender;
      s.miniData.descender = s.header.descender;
      s.miniData.is2Bit = s.header.is2Bit;
      s.miniData.ligaturePairs = s.ligaturePairs;
      s.miniData.ligaturePairCount = s.header.ligaturePairCount;
      s.miniData.provider = &styleProviders_[styleIdx];
      s.epdFont.data = &s.miniData;
      s.miniMode = PerStyle::MiniMode::METADATA;
    }
    // Subsequent calls: miniData is already wired; nothing to do.
    return 0;
  }

  // ---- Fast-path coverage check (SD fonts) ----
  // If the existing cache already covers all requested cps in a compatible mode,
  // there's nothing to do. This is the dominant case during pagination after the
  // first paragraph has populated the metadata cache.
  if (s.miniMode != PerStyle::MiniMode::NONE && allCpsCovered(s, codepoints, cpCount)) {
    // For metadata-only calls, METADATA or FULL cache both satisfy layout queries.
    // For full (bitmap) calls, only FULL satisfies — METADATA lacks bitmap data — and only at the
    // depth asked for: ink masks lack the grey levels, 2-bit glyphs the halving.
    if (metadataOnly || (s.miniMode == PerStyle::MiniMode::FULL && s.miniInkOnly == ink)) {
      // Ensure ligature metadata is wired if requested but not yet wired.
      if (loadKernLigatureData && !s.ligLoaded) {
        loadStyleKernLigatureData(s, /*ligatureOnly=*/true);
      }
      if (loadKernLigatureData && s.ligLoaded && s.miniData.ligaturePairs == nullptr) {
        if (s.miniMode == PerStyle::MiniMode::FULL) {
          applyKernLigaturePointers(s, s.miniData);
        } else {
          s.miniData.ligaturePairs = s.ligaturePairs;
          s.miniData.ligaturePairCount = s.header.ligaturePairCount;
        }
      }
      // Already wired into miniData; nothing else to do.
      return 0;
    }
  }

  // ---- Decide merge vs rebuild ----
  // Merge mode: extend the existing METADATA cache with the new cps without
  // re-reading already-loaded glyphs. Only safe when:
  //   - request is metadata-only (no bitmap data needed)
  //   - existing mode is METADATA
  //   - merged total stays within MAX_PAGE_GLYPHS
  // In all other cases we fall back to today's full-rebuild behavior (which is
  // also what happens for full bitmap prewarms — those are page-scoped).
  const bool tryMerge =
      metadataOnly && s.miniMode == PerStyle::MiniMode::METADATA && s.miniGlyphs != nullptr && s.miniGlyphCount > 0;

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };

  // Worst-case allocation: existing + new (for the merge path) or just new.
  const uint32_t mappingCapacity = tryMerge ? (s.miniGlyphCount + cpCount) : cpCount;
  CpGlyphMapping* mappings = allocArray<CpGlyphMapping>(mappingCapacity);
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;

  // For merge: seed mappings with already-loaded cps + their global indices,
  // recovered by walking miniIntervals and re-resolving via fullIntervals.
  // (We don't store globalIndex per mini-glyph; recomputing it is a binary
  // search per loaded cp, cheap relative to SD I/O.)
  if (tryMerge) {
    for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
      const auto& interval = s.miniIntervals[iv];
      for (uint32_t cp = interval.first; cp <= interval.last; cp++) {
        int32_t gIdx = findGlobalGlyphIndex(s, cp);
        if (gIdx < 0) continue;  // shouldn't happen; defensive
        mappings[validCount].codepoint = cp;
        mappings[validCount].globalIndex = gIdx;
        validCount++;
      }
    }
  }

  // Mark where the existing-glyph block ends; new cps follow.
  const uint32_t existingCount = validCount;

  // Add new requested cps that aren't already in the existing block.
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];

    // Skip cps already in the existing block (only relevant in merge mode).
    if (existingCount > 0) {
      bool dup = false;
      // Existing cps are sorted; binary search.
      int lo = 0, hi = static_cast<int>(existingCount) - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mappings[mid].codepoint < cp) {
          lo = mid + 1;
        } else if (mappings[mid].codepoint > cp) {
          hi = mid - 1;
        } else {
          dup = true;
          break;
        }
      }
      if (dup) continue;
    }

    int32_t idx = findGlobalGlyphIndex(s, cp);
    if (idx >= 0) {
      if (validCount >= MAX_PAGE_GLYPHS) {
        LOG_DBG("SDCF", "Cumulative cap (%u glyphs) reached for style %u; dropping merge cache", MAX_PAGE_GLYPHS,
                styleIdx);
        // Soft cap: discard the accumulated cache and fall back to rebuilding
        // with just the new request set. Caller's text will be covered;
        // already-loaded but no-longer-requested cps are lost.
        freeArray(mappings);
        freeStyleMiniData(s);
        // Recurse with rebuild semantics by clearing tryMerge state and trying
        // again — implemented as inline reset below.
        return prewarmStyle(styleIdx, codepoints, cpCount, metadataOnly, loadKernLigatureData);
      }
      mappings[validCount].codepoint = cp;
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  // Sort the full mappings array by codepoint (existing block is already sorted,
  // so std::sort on a partially sorted array is fast in practice; insertion-sort
  // would be optimal but std::sort is fine for our sizes).
  if (validCount > 0) {
    std::sort(mappings, mappings + validCount,
              [](const CpGlyphMapping& a, const CpGlyphMapping& b) { return a.codepoint < b.codepoint; });
  }

  // Count cps that are *newly* missing this accumulation cycle (i.e. not present
  // in mappings[] AND not previously reported). Already-reported misses are
  // suppressed so the same 4 special chars don't spam the log every paragraph.
  // `mappings[]` is sorted by codepoint after the std::sort above, enabling
  // O(log validCount) lookup per requested cp.
  int missed = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];
    int lo = 0, hi = static_cast<int>(validCount) - 1;
    bool found = false;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (mappings[mid].codepoint < cp) {
        lo = mid + 1;
      } else if (mappings[mid].codepoint > cp) {
        hi = mid - 1;
      } else {
        found = true;
        break;
      }
    }
    if (found) continue;

    // cp wasn't resolved. Check if we've already reported it this cycle.
    bool alreadyReported = false;
    for (uint8_t r = 0; r < s.reportedMissCount; r++) {
      if (s.reportedMisses[r] == cp) {
        alreadyReported = true;
        break;
      }
    }
    if (alreadyReported) continue;

    if (s.reportedMissCount < PerStyle::MAX_REPORTED_MISSES) {
      s.reportedMisses[s.reportedMissCount++] = cp;
    }
    missed++;
  }

  if (validCount == 0) {
    freeStyleMiniData(s);
    freeArray(mappings);
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Stash the old miniGlyphs so we can copy already-loaded entries by codepoint
  // before freeing them. (For merge path; nullptr when not merging.)
  EpdGlyph* oldGlyphs = tryMerge ? s.miniGlyphs : nullptr;
  EpdUnicodeInterval* oldIntervals = tryMerge ? s.miniIntervals : nullptr;
  uint32_t oldIntervalCount = tryMerge ? s.miniIntervalCount : 0;
  // Detach so freeStyleMiniData doesn't delete them yet.
  if (tryMerge) {
    s.miniIntervals = nullptr;
    s.miniGlyphs = nullptr;
    s.miniIntervalCount = 0;
    s.miniGlyphCount = 0;
  }

  // freeStyleMiniData wipes everything, including miniBitmap (which is nullptr
  // in metadata mode anyway). Safe in either path.
  freeStyleMiniData(s);

  uint32_t intervalCapacity = validCount;
  s.miniIntervals = allocArray<EpdUnicodeInterval>(intervalCapacity);
  if (!s.miniIntervals) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    freeArray(oldGlyphs);
    freeArray(oldIntervals);
    freeArray(mappings);
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

  // Allocate mini glyph array
  s.miniGlyphCount = validCount;
  s.miniGlyphs = allocArray<EpdGlyph>(s.miniGlyphCount);
  if (!s.miniGlyphs) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    freeArray(oldGlyphs);
    freeArray(oldIntervals);
    freeArray(mappings);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build a tracking array of which mappings still need SD I/O. For the merge
  // path, copy already-loaded glyph metadata from oldGlyphs first; remaining
  // entries get read from SD below.
  bool* needsRead = allocArray<bool>(validCount);
  if (!needsRead) {
    LOG_ERR("SDCF", "Failed to allocate needsRead array for style %u", styleIdx);
    freeArray(oldGlyphs);
    freeArray(oldIntervals);
    freeArray(mappings);
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) needsRead[i] = true;

  if (tryMerge && oldGlyphs && oldIntervals) {
    // For each cp in the new merged set, look it up in the old miniIntervals to
    // see if we already have its EpdGlyph. If yes, copy it over and skip the read.
    for (uint32_t i = 0; i < validCount; i++) {
      const uint32_t cp = mappings[i].codepoint;
      // Binary search oldIntervals for cp.
      int lo = 0, hi = static_cast<int>(oldIntervalCount) - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const auto& iv = oldIntervals[mid];
        if (cp < iv.first) {
          hi = mid - 1;
        } else if (cp > iv.last) {
          lo = mid + 1;
        } else {
          s.miniGlyphs[i] = oldGlyphs[iv.offset + (cp - iv.first)];
          needsRead[i] = false;
          break;
        }
      }
    }
  }

  freeArray(oldGlyphs);
  freeArray(oldIntervals);

  // Count how many SD reads are still needed, and build a sorted read order.
  uint32_t toReadCount = 0;
  for (uint32_t i = 0; i < validCount; i++) {
    if (needsRead[i]) toReadCount++;
  }

  uint32_t* readOrder = nullptr;
  if (toReadCount > 0) {
    readOrder = allocArray<uint32_t>(toReadCount);
    if (!readOrder) {
      LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
      freeArray(needsRead);
      freeArray(mappings);
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < validCount; i++) {
      if (needsRead[i]) readOrder[k++] = i;
    }
    std::sort(readOrder, readOrder + toReadCount,
              [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;
  FsFile file;

  if (toReadCount > 0) {
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
      freeArray(readOrder);
      freeArray(needsRead);
      freeArray(mappings);
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }

    // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
    // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
    // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
    // be false and we'd read from the file's current position — the header — which
    // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
    // containing that codepoint beyond page width).
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < toReadCount; i++) {
      uint32_t mapIdx = readOrder[i];
      int32_t gIdx = mappings[mapIdx].globalIndex;

      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        file.seekSet(fileOff);
        seekCount++;
      }
      if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
        file.close();
        freeArray(readOrder);
        freeArray(needsRead);
        freeArray(mappings);
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastReadIndex = gIdx;
    }
  }
  freeArray(needsRead);
  freeArray(readOrder);
  readOrder = nullptr;

  uint32_t totalBitmapSize = 0;
  bool bitmapsFromMmap = false;

  if (!metadataOnly) {
    // Compute total bitmap size: an ink-only prewarm keeps each 2-bit glyph's 1-bit ink mask.
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += ink ? (s.miniGlyphs[i].dataLength + 1u) / 2u : s.miniGlyphs[i].dataLength;
    }

    // Flash-mmap fast path: the whole .cpfont is already mapped, and glyph
    // bitmaps are raw bytes with no alignment requirement — the same property
    // that licenses the kern-matrix fast path in buildMiniKernMatrix(). Keep
    // each glyph's dataOffset exactly as read from the file and point miniData
    // at the mapped bitmap section, so the per-page arena is never allocated
    // and no bitmap byte is read from SD. Skipped for pure-SD fonts (mmap
    // unavailable), which keep the copy-into-arena path below.
    if (mmapDataBase_) {
      bitmapsFromMmap = true;
    } else {
      // The metadata pass above only opened the file for cps that needed a
      // metadata read — open it now if it isn't already.
      if (!file) {
        if (!Storage.openFileForRead("SDCF", filePath_, file)) {
          LOG_ERR("SDCF", "Failed to reopen .cpfont for bitmap prewarm (style %u)", styleIdx);
          freeArray(mappings);
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
      }

      // Recorded before the allocation, because the retry in prewarm() needs it precisely when
      // that allocation fails. Rounded up — see the field comment.
      if (validCount > 0) s.measuredBytesPerGlyph = (totalBitmapSize + validCount - 1) / validCount;

      s.miniBitmap = allocArray<uint8_t>(totalBitmapSize > 0 ? totalBitmapSize : 1);
      if (!s.miniBitmap) {
        LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
        file.close();
        freeArray(mappings);
        freeStyleMiniData(s);
        // Not a glyph count: this is one contiguous block, so it can fail with plenty of free
        // heap. Let the caller retry with fewer glyphs instead of losing the whole style.
        return PREWARM_ARENA_TOO_LARGE;
      }
      s.miniBitmapBytes = arena_ ? 0 : totalBitmapSize;

      // Allocate a fresh readOrder covering all validCount glyphs, sorted by
      // bitmap file offset for sequential I/O.
      uint32_t* bitmapOrder = allocArray<uint32_t>(validCount);
      if (!bitmapOrder) {
        LOG_ERR("SDCF", "Failed to allocate bitmap read order for style %u", styleIdx);
        file.close();
        freeArray(mappings);
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      for (uint32_t i = 0; i < validCount; i++) bitmapOrder[i] = i;
      std::sort(bitmapOrder, bitmapOrder + validCount,
                [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

      uint32_t miniBitmapOffset = 0;
      uint32_t lastBitmapEnd = UINT32_MAX;
      for (uint32_t i = 0; i < validCount; i++) {
        uint32_t mapIdx = bitmapOrder[i];
        EpdGlyph& glyph = s.miniGlyphs[mapIdx];

        if (glyph.dataLength == 0) {
          glyph.dataOffset = miniBitmapOffset;
          continue;
        }

        uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
        if (fileOff != lastBitmapEnd) {
          file.seekSet(fileOff);
          seekCount++;
        }
        bool readOk = true;
        uint32_t storedBytes = glyph.dataLength;
        if (ink) {
          // Even-sized chunks, so each pair of 2-bit bytes lands in one mask byte.
          uint8_t chunk[64];
          for (uint32_t done = 0; readOk && done < glyph.dataLength; done += sizeof(chunk)) {
            const uint32_t n = std::min<uint32_t>(sizeof(chunk), glyph.dataLength - done);
            readOk = file.read(chunk, n) == static_cast<int>(n);
            if (readOk) packInkMask(chunk, n, s.miniBitmap + miniBitmapOffset + done / 2);
          }
          storedBytes = (glyph.dataLength + 1u) / 2u;
        } else {
          readOk = file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) == static_cast<int>(glyph.dataLength);
        }
        if (!readOk) {
          LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
          file.close();
          freeArray(bitmapOrder);
          freeArray(mappings);
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        lastBitmapEnd = fileOff + glyph.dataLength;

        glyph.dataOffset = miniBitmapOffset;
        glyph.dataLength = static_cast<uint16_t>(storedBytes);
        miniBitmapOffset += storedBytes;
      }
      freeArray(bitmapOrder);
    }
  }

  uint32_t sdTime = millis() - sdStart;
  freeArray(mappings);

  // Kern/ligature wiring strategy:
  //   - Full render prewarm (!metadataOnly): load persistent kern classes +
  //     ligatures AND build the per-page mini kern matrix. The matrix is
  //     page-scoped — built once per render, amortized over the page draw.
  //   - Layout-only prewarm with loadKernLigatureData: load only the persistent
  //     kern classes + ligature pairs (cheap, idempotent) and skip the mini
  //     kern matrix. Ligatures are needed for correct word-width measurement
  //     (e.g. "fi" must measure as one glyph). Per-pair kern tweaks would be
  //     <1px adjustments and are too expensive to rebuild per paragraph
  //     (each rebuild does N seeks + reads against SD; observed cost ~24ms
  //     per style per call). EpdFont::getKerning() returns 0 cleanly when
  //     kernMatrix is null, so layout uses ligatures + zero-kern (small layout
  //     drift acceptable; full kerning is applied at page-render time).
  bool kernLigOk = false;
  if (!metadataOnly) {
    if (loadStyleKernLigatureData(s)) {
      // Reuse the file handle from the bitmap pass to avoid a redundant
      // Storage.openFileForRead() inside buildMiniKernMatrix (fix A).
      // Mmap fonts need no handle at all: buildMiniKernMatrix reads the matrix
      // straight from the mapping and never touches `file`, and the bitmap pass
      // above no longer opens one — so don't open it just to pass it in.
      if (!file && !mmapDataBase_) {
        if (!Storage.openFileForRead("SDCF", filePath_, file)) {
          LOG_ERR("SDCF", "Failed to open .cpfont for kern matrix (style %u)", styleIdx);
        } else {
          kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount, file);
        }
      } else {
        kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount, file);
      }
    }
  } else if (loadKernLigatureData) {
    loadStyleKernLigatureData(s, /*ligatureOnly=*/true);
    // Don't set kernLigOk → mini kern matrix stays null on miniData, but
    // ligatures are still resident on stubData (set in loadStyleKernLigatureData).
  }

  if (file) file.close();

  // Populate miniData and swap
  memset(&s.miniData, 0, sizeof(s.miniData));
  // On the mmap fast path miniBitmap stays null and each glyph's dataOffset is
  // still relative to the font's bitmap section, so the base is that section in
  // the flash mapping. EpdFontData::bitmap is a const pointer and built-in fonts
  // already point it at flash rodata, so the draw path is unchanged.
  s.miniData.bitmap = bitmapsFromMmap ? (mmapDataBase_ + s.bitmapFileOffset) : s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniInkOnly = ink;
  s.miniData.is2Bit = s.header.is2Bit && !ink;
  if (kernLigOk) {
    // Full prewarm: wire mini kern matrix + class tables + ligatures.
    applyKernLigaturePointers(s, s.miniData);
  } else if (loadKernLigatureData && s.ligLoaded) {
    // Layout-only prewarm: wire ligatures so applyLigatures() works (e.g. "fi"
    // measures correctly). Skip the kern matrix — getKerning() returns 0
    // cleanly when kernMatrix is null. Per-pair kern is applied at render time.
    s.miniData.ligaturePairs = s.ligaturePairs;
    s.miniData.ligaturePairCount = s.header.ligaturePairCount;
  }
  s.miniData.provider = &styleProviders_[styleIdx];

  s.epdFont.data = &s.miniData;
  s.miniMode = metadataOnly ? PerStyle::MiniMode::METADATA : PerStyle::MiniMode::FULL;

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
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    styles_[i].reportedMissCount = 0;
    applyGlyphMissCallback(i);
  }
}

size_t SdCardFont::pageArenaBytes() const {
  size_t bytes = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (styles_[i].present) bytes += styles_[i].miniBitmapBytes;
  }
  return bytes;
}

size_t SdCardFont::releasePageArenaIfAtLeast(const size_t minBytes) {
  const size_t bytes = pageArenaBytes();
  if (bytes == 0 || bytes < minBytes) return 0;
  clearCache();
  return bytes;
}

void SdCardFont::clearAccumulation() {
  // Same as clearCache() but skips the overflow ring buffer (per-glyph on-demand
  // loads are independent of the cumulative metadata cache). Also resets the
  // miss-report tracker so each section can re-report its missing cps once.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    styles_[i].reportedMissCount = 0;
    applyGlyphMissCallback(i);
  }
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  // Suppress when this font wasn't touched this phase — FontCacheManager iterates every
  // registered SD font, but only the active one for the page will have non-zero stats.
  if (stats_.prewarmTotalMs == 0 && stats_.sdReadTimeMs == 0 && stats_.seekCount == 0 && stats_.uniqueGlyphs == 0 &&
      stats_.bitmapBytes == 0) {
    return;
  }
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  if (style >= MAX_STYLES || !styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return style < MAX_STYLES && styles_[style].present; }

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<StyleProvider*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals) return nullptr;

  // Diagnostic (throttled): first few misses are logged verbosely, then every 64th miss.
  s.onDemandMissCount++;
  const bool logDetailedMiss = s.onDemandMissLogged < 12 || (s.onDemandMissCount % 64u) == 0u;
  if (logDetailedMiss) {
    LOG_DBG("SDCF", "onGlyphMiss: U+%04X style %u miniMode=%u miniIntervals=%u bitmap=%p miss#=%lu", codepoint,
            styleIdx, static_cast<uint8_t>(s.miniMode), s.miniIntervalCount, s.miniBitmap, s.onDemandMissCount);
    if (s.onDemandMissLogged == 12) {
      LOG_DBG("SDCF", "onGlyphMiss: suppressing verbose logs for style %u; logging every 64th miss", styleIdx);
    }
    s.onDemandMissLogged++;
  }

  // Under an ink-only page the font data says 1-bit, so the miss must be an ink mask too.
  const bool ink = s.miniInkOnly && s.epdFont.data == &s.miniData;

  // Check overflow cache first (matching codepoint, style and depth).
  // Probe all slots: the ring may be sparse after failed insertions.
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!self->overflow_[i].occupied) {
      continue;
    }
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx &&
        self->overflow_[i].inkOnly == ink) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails.
  uint32_t slot = self->overflowNext_;
  const bool overwriteOccupied = self->overflow_[slot].occupied;
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;

  // Read glyph metadata into temporary
  FsFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph;
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: seek failed for glyph metadata U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];  // heap even in arena mode (OverflowEntry)
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      file.close();
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: seek failed for bitmap U+%04X style %u", codepoint, styleIdx);
      delete[] tempBitmap;
      file.close();
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      file.close();
      return nullptr;
    }
  }

  file.close();

  if (ink && tempBitmap) {
    packInkMask(tempBitmap, tempGlyph.dataLength, tempBitmap);  // in place: the mask is the front half
    tempGlyph.dataLength = static_cast<uint16_t>((tempGlyph.dataLength + 1u) / 2u);
  }

  // All reads succeeded — commit to slot (evict old entry when overwriting an occupied slot).
  if (overwriteOccupied) {
    LOG_DBG("SDCF", "Overflow: evicting U+%04X style %u from slot %u", self->overflow_[slot].codepoint,
            self->overflow_[slot].styleIdx, slot);
    delete[] self->overflow_[slot].bitmap;
  } else if (self->overflowCount_ < OVERFLOW_CAPACITY) {
    self->overflowCount_++;
  }
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;
  self->overflow_[slot].inkOnly = ink;
  self->overflow_[slot].occupied = true;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}
