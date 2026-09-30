#include "ComplexShaper.h"

#include <Arduino.h>
#include <FontAlloc.h>
#include <Logging.h>
#include <OtShaper.h>
#include <Utf8.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "Fnv1a.h"
#include "ShapingTokens.h"

namespace {

// Recursive: source loaders run under the lock (from ensureFace) and allocate
// their buffers through allocate(), which takes it again.
std::recursive_mutex& shaperMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

// --- Budgeted allocator -----------------------------------------------------

// Each block carries its size in a header so realloc/free can keep the
// running total exact. The header keeps the platform's malloc alignment.
constexpr size_t kHeader = alignof(std::max_align_t) > sizeof(size_t) ? alignof(std::max_align_t) : sizeof(size_t);

#ifdef BOARD_HAS_PSRAM
constexpr size_t kDefaultBudget = 1024 * 1024;
constexpr size_t kInternalReserve = 0;
constexpr size_t kMaxLiveFaces = 8;
#else
// An Indic face's 5-80 KB of layout tables are flash-mapped (see
// FlashBlobCache.h); when they are not, they count here. When the budget runs
// out, appendShapedRun() drops every face and retries with just the one it
// needs. The reserve is free heap left to the section build and page render.
// It is counted against the total, not the largest block: shaping allocates
// at most a few KB at a time, and a fragmented C3 heap rarely has a 20 KB
// block to spare while reading.
constexpr size_t kDefaultBudget = 64 * 1024;
constexpr size_t kInternalReserve = 20 * 1024;
constexpr size_t kMaxLiveFaces = 2;  // regular + bold without rebuilding on every style change
#endif

size_t gBudget = kDefaultBudget;
uint32_t gShapedRuns = 0;
uint32_t gReusedRuns = 0;
size_t gCurrent = 0;
size_t gPeak = 0;
uint32_t gFailures = 0;
// Loads that failed for a reason that may pass: allocations refused
// (gFailures) plus sources that could not be read (noteSourceUnavailable).
uint32_t gUnavailable = 0;
uint32_t transientFailures() { return gFailures + gUnavailable; }

// `growth` is what the allocation adds to the heap in use; `block` is the
// contiguous size it needs (they differ for realloc).
bool admits(const size_t growth, const size_t block) {
  if (gCurrent + growth > gBudget) return false;
  if (kInternalReserve != 0 &&
      (ESP.getFreeHeap() < growth + kHeader + kInternalReserve || ESP.getMaxAllocHeap() < block + kHeader)) {
    return false;
  }
  return true;
}

void noteFailure([[maybe_unused]] const size_t size) {
  if ((gFailures++ & 0x3F) == 0) {
    LOG_ERR("SHAPE", "Allocation of %u bytes refused (in use %u / %u, heap free %u, max block %u)",
            static_cast<unsigned>(size), static_cast<unsigned>(gCurrent), static_cast<unsigned>(gBudget),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
}

void* budgetedMalloc(const size_t size) {
  if (size > gBudget || !admits(size, size)) {
    noteFailure(size);
    return nullptr;
  }
  auto* base = static_cast<uint8_t*>(fiFontMalloc(size + kHeader));
  if (base == nullptr) {
    noteFailure(size);
    return nullptr;
  }
  memcpy(base, &size, sizeof(size));
  gCurrent += size;
  if (gCurrent > gPeak) gPeak = gCurrent;
  return base + kHeader;
}

size_t blockSize(void* ptr) {
  size_t size;
  memcpy(&size, static_cast<uint8_t*>(ptr) - kHeader, sizeof(size));
  return size;
}

void budgetedFree(void* ptr) {
  if (ptr == nullptr) return;
  gCurrent -= blockSize(ptr);
  fiFontFree(static_cast<uint8_t*>(ptr) - kHeader);
}

void* budgetedRealloc(void* ptr, const size_t size) {
  if (ptr == nullptr) return budgetedMalloc(size);
  if (size == 0) {
    budgetedFree(ptr);
    return nullptr;
  }
  const size_t old = blockSize(ptr);
  if (size > old && (size > gBudget || !admits(size - old, size))) {
    noteFailure(size);
    return nullptr;
  }
  auto* base = static_cast<uint8_t*>(fiFontRealloc(static_cast<uint8_t*>(ptr) - kHeader, size + kHeader));
  if (base == nullptr) {
    noteFailure(size);
    return nullptr;
  }
  memcpy(base, &size, sizeof(size));
  gCurrent = gCurrent - old + size;
  if (gCurrent > gPeak) gPeak = gCurrent;
  return base + kHeader;
}

// --- Shaped-run cache -------------------------------------------------------

// Layout measures every word and the page render draws it again, so each run
// is shaped at least twice; common words recur throughout a chapter. A small
// direct-mapped cache over one fixed arena serves repeats without touching
// the allocator: when the arena fills it is simply reset.
#ifdef BOARD_HAS_PSRAM
constexpr uint32_t kCacheSlots = 256;
constexpr uint32_t kCacheArenaBytes = 32 * 1024;
#else
constexpr uint32_t kCacheSlots = 128;
constexpr uint32_t kCacheArenaBytes = 4 * 1024;
#endif

struct CacheSlot {
  const ComplexShaper* owner;
  uint32_t scale;
  uint32_t hash;
  uint32_t offset;
  uint16_t inLength;
  uint16_t outLength;
};

CacheSlot* gSlots = nullptr;
uint8_t* gArena = nullptr;
uint32_t gArenaHead = 0;

bool ensureCache() {
  if (gSlots != nullptr) return true;
  gSlots = static_cast<CacheSlot*>(budgetedMalloc(sizeof(CacheSlot) * kCacheSlots));
  gArena = static_cast<uint8_t*>(budgetedMalloc(kCacheArenaBytes));
  if (gSlots == nullptr || gArena == nullptr) {
    budgetedFree(gSlots);
    budgetedFree(gArena);
    gSlots = nullptr;
    gArena = nullptr;
    return false;
  }
  memset(gSlots, 0, sizeof(CacheSlot) * kCacheSlots);
  gArenaHead = 0;
  return true;
}

const CacheSlot* cacheFind(const ComplexShaper* owner, const uint32_t scale, const uint32_t hash, const char* run,
                           const size_t length) {
  if (gSlots == nullptr) return nullptr;
  const CacheSlot& slot = gSlots[hash % kCacheSlots];
  if (slot.owner != owner || slot.scale != scale || slot.hash != hash || slot.inLength != length) return nullptr;
  return memcmp(gArena + slot.offset, run, length) == 0 ? &slot : nullptr;
}

void cacheStore(const ComplexShaper* owner, const uint32_t scale, const uint32_t hash, const char* run,
                const size_t length, const char* shaped, const size_t shapedLength) {
  const size_t need = length + shapedLength;
  if (length > UINT16_MAX || shapedLength > UINT16_MAX || need > kCacheArenaBytes / 8) return;
  if (!ensureCache()) return;
  if (gArenaHead + need > kCacheArenaBytes) {
    memset(gSlots, 0, sizeof(CacheSlot) * kCacheSlots);
    gArenaHead = 0;
  }
  memcpy(gArena + gArenaHead, run, length);
  memcpy(gArena + gArenaHead + length, shaped, shapedLength);
  gSlots[hash % kCacheSlots] =
      CacheSlot{owner, scale, hash, gArenaHead, static_cast<uint16_t>(length), static_cast<uint16_t>(shapedLength)};
  gArenaHead += static_cast<uint32_t>(need);
}

// --- Layout memo --------------------------------------------------------------

// Unlike the direct-mapped cache, the memo never evicts: a paragraph measured
// in one pass and flattened into page-cache lines in the next shapes every run
// twice, and the gap between the two can exceed the cache. It also records
// every text that failed to shape (an entry with no output), so the second
// pass fails it too and draws the fallback the first pass measured. Entries
// are appended until the cap and then the memo simply stops growing.
#ifdef BOARD_HAS_PSRAM
constexpr uint32_t kMemoMaxEntries = 2048;
constexpr uint32_t kMemoMaxBytes = 128 * 1024;
#else
constexpr uint32_t kMemoMaxEntries = 384;
constexpr uint32_t kMemoMaxBytes = 16 * 1024;
#endif

CacheSlot* gMemo = nullptr;
uint32_t gMemoCount = 0;
uint32_t gMemoCapacity = 0;
uint8_t* gMemoArena = nullptr;
uint32_t gMemoBytes = 0;
uint32_t gMemoArenaCapacity = 0;
uint16_t gMemoDepth = 0;

// A shaped run, or with `failed` a text recorded as failing to shape.
const CacheSlot* memoFind(const ComplexShaper* owner, const uint32_t scale, const uint32_t hash, const char* run,
                          const size_t length, const bool failed = false) {
  for (uint32_t i = 0; i < gMemoCount; i++) {
    const CacheSlot& e = gMemo[i];
    if ((e.outLength == 0) == failed && e.hash == hash && e.owner == owner && e.scale == scale &&
        e.inLength == length && memcmp(gMemoArena + e.offset, run, length) == 0) {
      return &e;
    }
  }
  return nullptr;
}

void memoStore(const ComplexShaper* owner, const uint32_t scale, const uint32_t hash, const char* run,
               const size_t length, const char* shaped, const size_t shapedLength) {
  const size_t need = length + shapedLength;
  if (gMemoDepth == 0 || length > UINT16_MAX || shapedLength > UINT16_MAX) return;
  if (gMemoCount == kMemoMaxEntries || gMemoBytes + need > kMemoMaxBytes) return;
  if (gMemoCount == gMemoCapacity) {
    const uint32_t capacity = gMemoCapacity ? gMemoCapacity * 2 : 64;
    auto* grown = static_cast<CacheSlot*>(budgetedRealloc(gMemo, sizeof(CacheSlot) * capacity));
    if (grown == nullptr) return;
    gMemo = grown;
    gMemoCapacity = capacity;
  }
  if (gMemoBytes + need > gMemoArenaCapacity) {
    uint32_t capacity = gMemoArenaCapacity ? gMemoArenaCapacity * 2 : 2048;
    while (capacity < gMemoBytes + need) capacity *= 2;
    if (capacity > kMemoMaxBytes) capacity = kMemoMaxBytes;
    auto* grown = static_cast<uint8_t*>(budgetedRealloc(gMemoArena, capacity));
    if (grown == nullptr) return;
    gMemoArena = grown;
    gMemoArenaCapacity = capacity;
  }
  memcpy(gMemoArena + gMemoBytes, run, length);
  memcpy(gMemoArena + gMemoBytes + length, shaped, shapedLength);
  gMemo[gMemoCount++] =
      CacheSlot{owner, scale, hash, gMemoBytes, static_cast<uint16_t>(length), static_cast<uint16_t>(shapedLength)};
  gMemoBytes += static_cast<uint32_t>(need);
}

// OpenType language systems every run is shaped in (setDocumentLanguage).
const uint32_t* gLanguageTags = ot::languageTagsFor("");

// Drops every shaped run, keeping the cache and memo storage.
void forgetAllRuns() {
  if (gSlots != nullptr) memset(gSlots, 0, sizeof(CacheSlot) * kCacheSlots);
  gArenaHead = 0;
  gMemoCount = 0;
  gMemoBytes = 0;
}

void memoFree() {
  budgetedFree(gMemo);
  budgetedFree(gMemoArena);
  gMemo = nullptr;
  gMemoArena = nullptr;
  gMemoCount = gMemoCapacity = gMemoBytes = gMemoArenaCapacity = 0;
}

void cacheForget(const ComplexShaper* owner) {
  if (gSlots != nullptr) {
    for (uint32_t i = 0; i < kCacheSlots; i++) {
      if (gSlots[i].owner == owner) gSlots[i] = CacheSlot{};
    }
  }
  for (uint32_t i = 0; i < gMemoCount; i++) {
    if (gMemo[i].owner == owner) gMemo[i].owner = nullptr;  // never matches again
  }
}

// --- Runs -------------------------------------------------------------------

// One script's text plus the shared codepoints around it, ending at `end`.
// `script` is nullptr for shared codepoints alone.
struct Run {
  const unsigned char* end;
  const indic::ScriptInfo* script;
};

// The run starting at `p`; `end == p` when `p` is not Indic.
Run scanRun(const unsigned char* p) {
  const indic::ScriptInfo* script = nullptr;
  while (*p) {
    const unsigned char* next = p;
    const uint32_t cp = utf8NextCodepoint(&next);
    const indic::ScriptInfo* cpScript = indic::scriptOf(cp);
    if (cpScript == nullptr && !indic::isShared(cp)) break;
    if (cpScript != nullptr) {
      if (script != nullptr && cpScript != script) break;
      script = cpScript;
    }
    p = next;
  }
  return Run{p, script};
}

// 26.6 -> whole pixels, rounding half away from zero.
constexpr int roundPixels(const int32_t v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

void appendToken(const uint32_t cp, std::string& out) { utf8AppendCodepoint(cp, out); }

constexpr uint16_t kRetryAfterFailure = 64;
constexpr uint32_t kRetryAfterMs = 1000;

// A shaping plan (the font's lookup list for one script, a few KB) is only
// built when the shaping reserve remains free around it.
constexpr size_t kPlanReserve = kInternalReserve;

// The tables shaping reads, in ot::FaceTables order.
constexpr const uint32_t* kShapingTables = ot::FaceTables::TAGS;
constexpr size_t kTableCount = ot::FaceTables::COUNT;
constexpr size_t kRequiredTables = ot::FaceTables::CMAP + 1;  // head..cmap; the layout tables are optional

// Glyph string and codepoints of the run being shaped, shared by every
// shaper (shaping holds the lock) and kept between runs.
ot::Buffer* gBuffer = nullptr;
std::vector<uint32_t>* gCodepoints = nullptr;

size_t gPlanBytes = 0;

ComplexShaper* gShapers = nullptr;

}  // namespace

// One font's tables and shaping plans, shared by every shaper whose source
// carries the same font.
struct SharedFace {
  uint32_t key;                  // 0 = private to one shaper
  ComplexShaper::Blob blob;      // whole layout font (blob sources)
  uint8_t* tables[kTableCount];  // loaded tables (table sources), from allocate()
  ot::Face face;
  // Fills a plan for this face: read from the font (blobs) or built (tables).
  bool (*makePlan)(ot::Plan& plan, const ot::Face& face, ot::Script script, const uint32_t* languageTags);
  ot::Plan* plans[indic::SCRIPT_COUNT];  // made on demand in the document language
  uint16_t users;
  SharedFace* next;
};

namespace {

SharedFace* gFaces = nullptr;

SharedFace* findFace(const uint32_t key) {
  if (key == 0) return nullptr;
  for (SharedFace* f = gFaces; f != nullptr; f = f->next) {
    if (f->key == key) return f;
  }
  return nullptr;
}

void freePlans(SharedFace* shared) {
  for (ot::Plan*& plan : shared->plans) {
    if (plan == nullptr) continue;
    gPlanBytes -= plan->memoryBytes();
    delete plan;
    plan = nullptr;
  }
}

void destroyFace(SharedFace* shared) {
  freePlans(shared);
  if (shared->blob.data != nullptr) {
    auto* data = const_cast<uint8_t*>(shared->blob.data);
    if (shared->blob.release) {
      shared->blob.release(data);
    } else {
      budgetedFree(data);
    }
  }
  for (uint8_t* table : shared->tables) budgetedFree(table);
  shared->~SharedFace();
  budgetedFree(shared);
}

SharedFace* newFace(const uint32_t key) {
  void* memory = budgetedMalloc(sizeof(SharedFace));
  if (memory == nullptr) return nullptr;
  auto* shared = new (memory) SharedFace{};
  shared->key = key;
  return shared;
}

void dropFace(SharedFace* shared) {
  if (--shared->users != 0) return;
  for (SharedFace** link = &gFaces; *link != nullptr; link = &(*link)->next) {
    if (*link == shared) {
      *link = shared->next;
      break;
    }
  }
  destroyFace(shared);
}

size_t liveFaceCount() {
  size_t count = 0;
  for (const SharedFace* f = gFaces; f != nullptr; f = f->next) count++;
  return count;
}

// Blob faces carry their plans compiled by the .cpfont converter; table
// faces (TTF/OTF files) plan at runtime. Only buildTableFace() refers to the
// runtime planner, and only setTableSource() to buildTableFace(), so builds
// without TTF support link neither.
bool loadCompiledPlan(ot::Plan& plan, const ot::Face& face, const ot::Script script, const uint32_t* languageTags) {
  return plan.load(face, script, languageTags);
}
bool buildPlan(ot::Plan& plan, const ot::Face& face, const ot::Script script, const uint32_t* languageTags) {
  return plan.build(face, script, languageTags);
}

// Builds a face from a table source. `key` receives the source's identity: a
// hash of its 'head' table, which carries the whole-file checksum and
// timestamps; an existing face with that key is returned instead.
SharedFace* buildTableFace(const ComplexShaper::TableLoader loader, void* ctx, uint32_t* key) {
  uint32_t lengths[kTableCount] = {};
  uint8_t* head = loader(ctx, kShapingTables[0], &lengths[0]);
  if (head == nullptr) return nullptr;
  *key = fnv1a::hash(head, lengths[0]) | 1u;  // never 0
  if (SharedFace* existing = findFace(*key)) {
    budgetedFree(head);
    return existing;
  }
  SharedFace* shared = newFace(*key);
  if (shared == nullptr) {
    budgetedFree(head);
    return nullptr;
  }
  shared->tables[0] = head;
  // An optional table the loader returns no data for is absent, unless the
  // load failed: then the face would silently shape without it.
  const uint32_t failuresBefore = transientFailures();
  for (size_t i = 1; i < kTableCount; i++) {
    shared->tables[i] = loader(ctx, kShapingTables[i], &lengths[i]);
    if (shared->tables[i] == nullptr && (i < kRequiredTables || transientFailures() != failuresBefore)) {
      destroyFace(shared);
      return nullptr;
    }
  }
  ot::FaceTables tables;
  for (size_t i = 0; i < kTableCount; i++) {
    if (shared->tables[i] != nullptr) tables.tables[i] = ot::Table(shared->tables[i], lengths[i]);
  }
  if (!shared->face.init(tables)) {
    destroyFace(shared);
    return nullptr;
  }
  shared->makePlan = &buildPlan;
  return shared;
}

SharedFace* buildBlobFace(const ComplexShaper::BlobLoader loader, void* ctx, const uint32_t key) {
  ComplexShaper::Blob source;
  if (!loader(ctx, &source)) return nullptr;
  SharedFace* shared = newFace(key);
  if (shared == nullptr) {
    if (source.release) {
      source.release(const_cast<uint8_t*>(source.data));
    } else {
      budgetedFree(const_cast<uint8_t*>(source.data));
    }
    return nullptr;
  }
  shared->blob = source;
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(source.data, source.length, &tables) || !shared->face.init(tables)) {
    destroyFace(shared);
    return nullptr;
  }
  shared->makePlan = &loadCompiledPlan;
  return shared;
}

// The face's plan for `script` in the document language, made on first use.
// ot::HeapCheck for the plan, its computed lookup filters and the glyph
// buffer: they must leave the plan reserve free, and fit a free block.
bool filtersFit(const size_t bytes) {
  return kPlanReserve == 0 ||
         (ESP.getFreeHeap() >= bytes + kPlanReserve + bytes / 4 && ESP.getMaxAllocHeap() >= bytes + bytes / 4);
}

const ot::Plan* planFor(SharedFace* shared, const indic::ScriptInfo& script) {
  ot::Plan*& plan = shared->plans[indic::indexOf(script)];
  if (plan != nullptr) return plan;
  ot::setHeapCheck(filtersFit);
  if (kPlanReserve != 0 && ESP.getFreeHeap() < kPlanReserve) {
    gFailures++;
    return nullptr;
  }
  plan = new (std::nothrow) ot::Plan();
  if (plan == nullptr) {
    gFailures++;
    return nullptr;
  }
  if (!shared->makePlan(*plan, shared->face, static_cast<ot::Script>(indic::indexOf(script)), gLanguageTags)) {
    delete plan;
    plan = nullptr;
    gFailures++;
    return nullptr;
  }
  gPlanBytes += plan->memoryBytes();
  return plan;
}

void releasePlansEverywhere() {
  for (SharedFace* f = gFaces; f != nullptr; f = f->next) freePlans(f);
}

void releaseBuffer() {
  delete gBuffer;
  delete gCodepoints;
  gBuffer = nullptr;
  gCodepoints = nullptr;
}

}  // namespace

ComplexShaper::ComplexShaper() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  next_ = gShapers;
  if (gShapers != nullptr) gShapers->prev_ = this;
  gShapers = this;
}

ComplexShaper::~ComplexShaper() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  releaseLocked();
  cacheForget(this);
  if (prev_ != nullptr) prev_->next_ = next_;
  if (next_ != nullptr) next_->prev_ = prev_;
  if (gShapers == this) gShapers = next_;
}

void ComplexShaper::setBlobSource(const BlobLoader loader, void* ctx, const uint32_t contentKey) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  releaseLocked();
  cacheForget(this);
  blobLoader_ = loader;
  tableLoader_ = nullptr;
  sourceCtx_ = ctx;
  contentKey_ = contentKey;
  unusable_ = false;
  coverageChecked_ = coverageMask_ = 0;
}

void ComplexShaper::setTableSource(const TableLoader loader, void* ctx) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  releaseLocked();
  cacheForget(this);
  tableLoader_ = loader;
  buildTableFace_ = &buildTableFace;
  blobLoader_ = nullptr;
  sourceCtx_ = ctx;
  contentKey_ = 0;
  unusable_ = false;
  coverageChecked_ = coverageMask_ = 0;
}

void ComplexShaper::setAdvanceSource(const AdvanceSource source, void* ctx) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  if (source == advanceSource_ && ctx == advanceCtx_) return;
  advanceSource_ = source;
  advanceCtx_ = ctx;
  cacheForget(this);
}

void ComplexShaper::setScale(const uint32_t ppem26_6) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  if (ppem26_6 == scale26_6_) return;
  scale26_6_ = ppem26_6;
  cacheForget(this);
}

bool ComplexShaper::ensureFace() {
  if (face_ != nullptr) return true;
  if (unusable_ || !hasSource() || scale26_6_ == 0) return false;
  if (retryBackoff_ > 0) {
    if (static_cast<uint32_t>(millis()) - backoffStartMs_ < kRetryAfterMs) {
      retryBackoff_--;
      return false;
    }
    retryBackoff_ = 0;
  }

  SharedFace* shared = findFace(contentKey_);
  if (shared == nullptr && liveFaceCount() >= kMaxLiveFaces) {
    // Tight heap: make room by dropping every other face. Their shapers
    // rebuild on their next run; the shaped-run cache still serves repeats.
    for (ComplexShaper* other = gShapers; other != nullptr; other = other->next_) {
      if (other != this) other->releaseLocked();
    }
  }
  if (shared == nullptr) {
    const uint32_t failuresBefore = transientFailures();
    uint32_t key = contentKey_;
    SharedFace* built = blobLoader_ != nullptr ? buildBlobFace(blobLoader_, sourceCtx_, key)
                                               : buildTableFace_(tableLoader_, sourceCtx_, &key);
    if (built == nullptr) {
      if (transientFailures() == failuresBefore) {
        LOG_ERR("SHAPE", "Shaping source is malformed; shaping disabled for this face");
        unusable_ = true;
      } else {
        retryBackoff_ = kRetryAfterFailure;
        backoffStartMs_ = static_cast<uint32_t>(millis());
      }
      return false;
    }
    shared = built;
    if (shared->users == 0 && findFace(shared->key) != shared) {
      shared->next = gFaces;
      gFaces = shared;
    }
  }
  shared->users++;
  face_ = shared;
  LOG_DBG("SHAPE", "Face ready at %u/64 ppem: %u glyphs, shared by %u, %u bytes in use", scale26_6_,
          static_cast<unsigned>(face_->face.glyphCount()), face_->users, static_cast<unsigned>(gCurrent));
  return true;
}

bool ComplexShaper::ensureFaceMakingRoom() {
  const uint32_t failuresBefore = gFailures;
  if (ensureFace()) return true;
  if (gFailures == failuresBefore) return false;  // not for lack of memory
  releaseEveryFaceLocked();
  retryBackoff_ = 0;
  return ensureFace();
}

void ComplexShaper::releaseEveryFaceLocked() {
  for (ComplexShaper* shaper = gShapers; shaper != nullptr; shaper = shaper->next_) shaper->releaseLocked();
}

ComplexShaper::Coverage ComplexShaper::coverage(const indic::ScriptInfo& script) {
  const auto bit = static_cast<uint16_t>(1u << indic::indexOf(script));
  if ((coverageChecked_ & bit) == 0) {
    if (!ensureFaceMakingRoom()) return Coverage::Unknown;
    uint32_t glyph = 0;
    if (face_->face.nominalGlyph(script.probe, &glyph)) coverageMask_ |= bit;
    coverageChecked_ |= bit;
  }
  return (coverageMask_ & bit) != 0 ? Coverage::Covered : Coverage::NotCovered;
}

bool ComplexShaper::appendShapedRun(const char* run, const size_t length, const indic::ScriptInfo& script,
                                    std::string& out) {
  const uint32_t hash = fnv1a::hash(run, length);
  if (const CacheSlot* hit = memoFind(this, scale26_6_, hash, run, length)) {
    out.append(reinterpret_cast<const char*>(gMemoArena + hit->offset + hit->inLength), hit->outLength);
    gReusedRuns++;
    return true;
  }
  if (const CacheSlot* hit = cacheFind(this, scale26_6_, hash, run, length)) {
    out.append(reinterpret_cast<const char*>(gArena + hit->offset + hit->inLength), hit->outLength);
    gReusedRuns++;
    memoStore(this, scale26_6_, hash, run, length, out.data() + out.size() - hit->outLength, hit->outLength);
    return true;
  }

  if (!ensureFaceMakingRoom()) return false;
  const ot::Plan* plan = planFor(face_, script);
  if (plan == nullptr) return false;
  if (gBuffer == nullptr) {
    gBuffer = new (std::nothrow) ot::Buffer();
    gCodepoints = new (std::nothrow) std::vector<uint32_t>();
    if (gBuffer == nullptr || gCodepoints == nullptr) {
      releaseBuffer();
      gFailures++;
      return false;
    }
  }
  std::vector<uint32_t>& codepoints = *gCodepoints;
  codepoints.clear();
  codepoints.reserve(length);
  const auto* p = reinterpret_cast<const unsigned char*>(run);
  const auto* end = p + length;
  while (p < end) codepoints.push_back(utf8NextCodepoint(&p));

  ot::Buffer& buffer = *gBuffer;
  ot::Scale scale;
  scale.set(static_cast<int32_t>(scale26_6_), (scale26_6_ + 32) >> 6, face_->face.upem());
  const bool shaped =
      ot::shape(face_->face, scale, *plan, codepoints.data(), static_cast<unsigned>(codepoints.size()), buffer);
  gShapedRuns++;
  if (!shaped) return false;

  const size_t start = out.size();
  for (unsigned i = 0; i < buffer.len(); i++) {
    const ot::GlyphPosition& pos = buffer.pos[i];
    const uint32_t glyph = buffer.info[i].codepoint;
    int32_t xAdvance = pos.xAdvance;
    // Keep what GPOS added to the hmtx advance; marks it zeroed stay zero.
    if (advanceSource_ != nullptr && !(buffer.info[i].isMark() && xAdvance == 0)) {
      const int32_t own = advanceSource_(advanceCtx_, glyph);
      if (own >= 0) xAdvance += own - scale.emScaleX(face_->face.advance(glyph));
    }
    // 26.6 -> 12.4, rounded.
    appendToken(shaping::advanceToken((xAdvance + 2) >> 2), out);
    const int dx = roundPixels(pos.xOffset);
    const int dy = -roundPixels(pos.yOffset);  // font y grows up, the screen's down
    if (dx != 0 || dy != 0) appendToken(shaping::offsetToken(dx, dy), out);
    const uint32_t gid = glyph <= shaping::GLYPH_TOKEN_MAX_GID ? glyph : 0;
    appendToken(shaping::glyphToken(gid), out);
  }
  cacheStore(this, scale26_6_, hash, run, length, out.data() + start, out.size() - start);
  memoStore(this, scale26_6_, hash, run, length, out.data() + start, out.size() - start);
  return true;
}

bool ComplexShaper::shape(const char* utf8, std::string& out) {
  if (!containsComplexScript(utf8)) return false;
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  if (unusable_ || !hasSource() || scale26_6_ == 0) return false;
  if (gMemoDepth == 0) return shapeRuns(utf8, out);

  const size_t length = strlen(utf8);
  const uint32_t hash = fnv1a::hash(utf8, length);
  if (memoFind(this, scale26_6_, hash, utf8, length, /*failed=*/true) != nullptr) return false;
  if (shapeRuns(utf8, out)) return true;
  memoStore(this, scale26_6_, hash, utf8, length, "", 0);
  return false;
}

bool ComplexShaper::shapeRuns(const char* utf8, std::string& out) {
  out.clear();
  out.reserve(strlen(utf8) * 4);
  bool shapedAny = false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    const Run run = scanRun(p);
    if (run.end == p) {
      const unsigned char* next = p;
      utf8NextCodepoint(&next);
      out.append(reinterpret_cast<const char*>(p), next - p);
      p = next;
      continue;
    }
    const auto* runText = reinterpret_cast<const char*>(p);
    const auto length = static_cast<size_t>(run.end - p);
    p = run.end;

    // Shared codepoints alone (a lone danda) and scripts the layout tables do
    // not cover stay text, drawn glyph by glyph like any unshaped string.
    const Coverage covered = run.script != nullptr ? coverage(*run.script) : Coverage::NotCovered;
    if (covered == Coverage::Unknown) return false;
    if (covered == Coverage::NotCovered) {
      out.append(runText, length);
      continue;
    }
    if (!appendShapedRun(runText, length, *run.script, out)) return false;
    shapedAny = true;
  }
  return shapedAny;
}

void ComplexShaper::releaseLocked() {
  // Cached runs stay valid: the same source and scale shape them identically
  // after a rebuild. They are forgotten when either changes (or on destruction).
  if (face_ != nullptr) dropFace(face_);
  face_ = nullptr;
}

void ComplexShaper::release() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  releaseLocked();
}

void* ComplexShaper::allocate(const size_t size) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  return budgetedMalloc(size);
}

void ComplexShaper::noteSourceUnavailable() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  gUnavailable++;
}

void ComplexShaper::deallocate(void* ptr) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  budgetedFree(ptr);
}

ComplexShaper::MemoryStats ComplexShaper::memoryStats() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  return MemoryStats{gCurrent + gPlanBytes, gPeak + gPlanBytes, gBudget, gFailures, gShapedRuns, gReusedRuns};
}

void ComplexShaper::setMemoryBudget(const size_t bytes) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  gBudget = bytes;
}

void ComplexShaper::releaseCache() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  budgetedFree(gSlots);
  budgetedFree(gArena);
  gSlots = nullptr;
  gArena = nullptr;
  gArenaHead = 0;
}

size_t ComplexShaper::releaseAll() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  const size_t before = gCurrent + gPlanBytes;
  releaseEveryFaceLocked();
  releaseCache();
  releaseBuffer();
  // The layout memo stays until its scope ends: dropping it would let the
  // scope's second pass shape differently from its first.
  return before - (gCurrent + gPlanBytes);
}

void ComplexShaper::setDocumentLanguage(const char* bcp47) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  const uint32_t* tags = ot::languageTagsFor(bcp47);
  if (tags == gLanguageTags) return;
  gLanguageTags = tags;
  releasePlansEverywhere();
  forgetAllRuns();
}

void ComplexShaper::beginMemo() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  gMemoDepth++;
}

void ComplexShaper::endMemo() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  if (gMemoDepth == 0 || --gMemoDepth != 0) return;
  memoFree();
}
