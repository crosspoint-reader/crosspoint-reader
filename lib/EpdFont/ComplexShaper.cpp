#include "ComplexShaper.h"

#include <Arduino.h>
#include <FontAlloc.h>
#include <Logging.h>
#include <Utf8.h>
#include <hb-ot.h>
#include <hb.h>

#include <cstddef>
#include <cstring>
#include <mutex>

#include "ShapingTokens.h"

namespace {

// Recursive: source loaders run under the lock (from ensureFont) and allocate
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
// One Bengali face is ~16 KB of HarfBuzz state (its ~40 KB of layout tables
// are flash-mapped, see FlashBlobCache.h, or else count here too). When the
// budget runs out anyway, appendShapedRun() drops every face and retries with
// just the one it needs. The reserve keeps shaping from taking the last
// contiguous block that section builds and page renders depend on.
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

bool admits(const size_t growth) {
  if (gCurrent + growth > gBudget) return false;
  if (kInternalReserve != 0 && ESP.getMaxAllocHeap() < growth + kHeader + kInternalReserve) {
    return false;
  }
  return true;
}

void noteFailure([[maybe_unused]] const size_t size) {
  if ((gFailures++ & 0x3F) == 0) {
    LOG_ERR("SHAPE", "Allocation of %u bytes refused (in use %u / %u)", static_cast<unsigned>(size),
            static_cast<unsigned>(gCurrent), static_cast<unsigned>(gBudget));
  }
}

void* budgetedMalloc(const size_t size) {
  if (size > gBudget || !admits(size)) {
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
  if (size > old && (size > gBudget || !admits(size - old))) {
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

uint32_t fnv1a(const char* data, const size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; i++) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

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
// twice, and the gap between the two can exceed the cache. Entries are
// appended until the cap and then the memo simply stops growing.
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

const CacheSlot* memoFind(const ComplexShaper* owner, const uint32_t scale, const uint32_t hash, const char* run,
                          const size_t length) {
  for (uint32_t i = 0; i < gMemoCount; i++) {
    const CacheSlot& e = gMemo[i];
    if (e.hash == hash && e.owner == owner && e.scale == scale && e.inLength == length &&
        memcmp(gMemoArena + e.offset, run, length) == 0) {
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

constexpr bool isRunCodepoint(const uint32_t cp) {
  return (cp >= 0x0980 && cp <= 0x09FF)   // Bengali
         || cp == 0x0964 || cp == 0x0965  // danda, double danda
         || cp == 0x200C || cp == 0x200D  // ZWNJ, ZWJ
         || cp == 0x25CC;                 // dotted circle
}

// 26.6 -> whole pixels, rounding half away from zero.
constexpr int roundPixels(const int32_t v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

void appendToken(const uint32_t cp, std::string& out) { utf8AppendCodepoint(cp, out); }

void destroyAllocated(void* data) { budgetedFree(data); }

hb_blob_t* ownedBlob(uint8_t* data, const uint32_t length) {
  return hb_blob_create(reinterpret_cast<const char*>(data), length, HB_MEMORY_MODE_READONLY, data, destroyAllocated);
}

// What HarfBuzz reads to shape with its OpenType font functions. Loaded up
// front so a face never calls back into the font object that built it.
constexpr hb_tag_t kShapingTables[] = {
    HB_TAG('h', 'e', 'a', 'd'), HB_TAG('h', 'h', 'e', 'a'), HB_TAG('m', 'a', 'x', 'p'), HB_TAG('h', 'm', 't', 'x'),
    HB_TAG('c', 'm', 'a', 'p'), HB_TAG('G', 'D', 'E', 'F'), HB_TAG('G', 'S', 'U', 'B'), HB_TAG('G', 'P', 'O', 'S'),
};
constexpr size_t kRequiredTables = 5;  // head..cmap; the layout tables are optional

constexpr uint16_t kRetryAfterFailure = 64;

ComplexShaper* gShapers = nullptr;

}  // namespace

// A HarfBuzz face and the shapers using it, keyed by source content.
struct SharedFace {
  uint32_t key;  // 0 = private to one shaper
  hb_face_t* face;
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

SharedFace* adoptFace(const uint32_t key, hb_face_t* face) {
  auto* shared = static_cast<SharedFace*>(budgetedMalloc(sizeof(SharedFace)));
  if (shared == nullptr) {
    hb_face_destroy(face);
    return nullptr;
  }
  *shared = SharedFace{key, face, 0, gFaces};
  gFaces = shared;
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
  hb_face_destroy(shared->face);
  budgetedFree(shared);
}

size_t liveFaceCount() {
  size_t count = 0;
  for (const SharedFace* f = gFaces; f != nullptr; f = f->next) count++;
  return count;
}

// Builds a self-contained face from a table source. `key` receives the
// source's identity: a hash of its 'head' table, which carries the whole-file
// checksum and timestamps.
hb_face_t* buildTableFace(const ComplexShaper::TableLoader loader, void* ctx, uint32_t* key) {
  uint32_t headLength = 0;
  uint8_t* head = loader(ctx, kShapingTables[0], &headLength);
  if (head == nullptr) return nullptr;
  *key = fnv1a(reinterpret_cast<const char*>(head), headLength) | 1u;  // never 0
  if (SharedFace* existing = findFace(*key)) {
    budgetedFree(head);
    return hb_face_reference(existing->face);
  }
  hb_face_t* face = hb_face_builder_create();
  hb_blob_t* headBlob = ownedBlob(head, headLength);
  hb_face_builder_add_table(face, kShapingTables[0], headBlob);
  hb_blob_destroy(headBlob);
  for (size_t i = 1; i < sizeof(kShapingTables) / sizeof(kShapingTables[0]); i++) {
    uint32_t length = 0;
    uint8_t* data = loader(ctx, kShapingTables[i], &length);
    if (data == nullptr) {
      if (i < kRequiredTables) {
        hb_face_destroy(face);
        return nullptr;
      }
      continue;
    }
    hb_blob_t* blob = ownedBlob(data, length);
    hb_face_builder_add_table(face, kShapingTables[i], blob);
    hb_blob_destroy(blob);
  }
  return face;
}

}  // namespace

extern "C" void* crosspointHbMalloc(const size_t size) { return budgetedMalloc(size); }
extern "C" void* crosspointHbCalloc(const size_t count, const size_t size) {
  if (size != 0 && count > SIZE_MAX / size) return nullptr;
  void* ptr = budgetedMalloc(count * size);
  if (ptr != nullptr) memset(ptr, 0, count * size);
  return ptr;
}
extern "C" void* crosspointHbRealloc(void* ptr, const size_t size) { return budgetedRealloc(ptr, size); }
extern "C" void crosspointHbFree(void* ptr) { budgetedFree(ptr); }

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
}

void ComplexShaper::setTableSource(const TableLoader loader, void* ctx) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  releaseLocked();
  cacheForget(this);
  tableLoader_ = loader;
  blobLoader_ = nullptr;
  sourceCtx_ = ctx;
  contentKey_ = 0;
  unusable_ = false;
}

void ComplexShaper::setScale(const uint32_t ppem26_6) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  if (ppem26_6 == scale26_6_) return;
  scale26_6_ = ppem26_6;
  cacheForget(this);
  if (font_ != nullptr) {
    hb_font_set_scale(font_, static_cast<int>(scale26_6_), static_cast<int>(scale26_6_));
    hb_font_set_ppem(font_, (scale26_6_ + 32) >> 6, (scale26_6_ + 32) >> 6);
  }
}

bool ComplexShaper::ensureFont() {
  if (font_ != nullptr) return true;
  if (unusable_ || !hasSource() || scale26_6_ == 0) return false;
  if (retryBackoff_ > 0) {
    retryBackoff_--;
    return false;
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
    hb_face_t* face = nullptr;
    uint32_t key = contentKey_;
    if (blobLoader_ != nullptr) {
      Blob source;
      if (blobLoader_(sourceCtx_, &source)) {
        hb_blob_t* blob =
            hb_blob_create(reinterpret_cast<const char*>(source.data), source.length, HB_MEMORY_MODE_READONLY,
                           const_cast<uint8_t*>(source.data), source.release ? source.release : destroyAllocated);
        face = hb_face_create(blob, 0);
        hb_blob_destroy(blob);
      }
    } else {
      face = buildTableFace(tableLoader_, sourceCtx_, &key);
    }
    if (face == nullptr || face == hb_face_get_empty()) {
      retryBackoff_ = kRetryAfterFailure;
      return false;
    }
    shared = findFace(key);  // a table source may resolve to a face another size already built
    if (shared != nullptr) {
      hb_face_destroy(face);
    } else {
      if (hb_face_get_glyph_count(face) == 0) {
        LOG_ERR("SHAPE", "Shaping source has no glyphs; shaping disabled for this face");
        hb_face_destroy(face);
        unusable_ = true;
        return false;
      }
      shared = adoptFace(key, face);
      if (shared == nullptr) {
        retryBackoff_ = kRetryAfterFailure;
        return false;
      }
    }
  }
  shared->users++;
  face_ = shared;

  font_ = hb_font_create(face_->face);
  buffer_ = hb_buffer_create();
  if (font_ == hb_font_get_empty() || !hb_buffer_allocation_successful(buffer_)) {
    releaseLocked();
    retryBackoff_ = kRetryAfterFailure;
    return false;
  }
  hb_font_set_scale(font_, static_cast<int>(scale26_6_), static_cast<int>(scale26_6_));
  hb_font_set_ppem(font_, (scale26_6_ + 32) >> 6, (scale26_6_ + 32) >> 6);
  LOG_DBG("SHAPE", "Font ready at %u/64 ppem: %u glyphs, face shared by %u, %u bytes in use", scale26_6_,
          hb_face_get_glyph_count(face_->face), face_->users, static_cast<unsigned>(gCurrent));
  return true;
}

bool ComplexShaper::appendShapedRun(const char* run, const size_t length, std::string& out) {
  const uint32_t hash = fnv1a(run, length);
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

  static hb_language_t bengali = hb_language_from_string("bn", -1);
  for (int attempt = 0;; attempt++) {
    const uint32_t failuresBefore = gFailures;
    if (ensureFont()) {
      hb_buffer_clear_contents(buffer_);
      hb_buffer_add_utf8(buffer_, run, static_cast<int>(length), 0, static_cast<int>(length));
      hb_buffer_set_direction(buffer_, HB_DIRECTION_LTR);
      hb_buffer_set_script(buffer_, HB_SCRIPT_BENGALI);
      hb_buffer_set_language(buffer_, bengali);
      hb_shape(font_, buffer_, nullptr, 0);
      gShapedRuns++;
      if (gFailures == failuresBefore && hb_buffer_allocation_successful(buffer_)) break;
    }
    if (gFailures == failuresBefore || attempt > 0) return false;
    // An allocation failed inside HarfBuzz, which caches the tables it fails
    // to build: shaping with this face now could silently skip substitutions.
    // Drop every face (the shaped-run cache survives) and rebuild just this one.
    for (ComplexShaper* shaper = gShapers; shaper != nullptr; shaper = shaper->next_) shaper->releaseLocked();
    retryBackoff_ = 0;
  }

  unsigned int count = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer_, &count);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer_, nullptr);
  const size_t start = out.size();
  for (unsigned int i = 0; i < count; i++) {
    const hb_glyph_position_t& pos = positions[i];
    // 26.6 -> 12.4, rounded.
    appendToken(shaping::advanceToken((pos.x_advance + 2) >> 2), out);
    const int dx = roundPixels(pos.x_offset);
    const int dy = -roundPixels(pos.y_offset);  // HarfBuzz y grows up, the screen's down
    if (dx != 0 || dy != 0) appendToken(shaping::offsetToken(dx, dy), out);
    const uint32_t gid = infos[i].codepoint <= shaping::GLYPH_TOKEN_MAX_GID ? infos[i].codepoint : 0;
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

  out.clear();
  out.reserve(strlen(utf8) * 4);
  bool shapedAny = false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    const unsigned char* next = p;
    if (!isRunCodepoint(utf8NextCodepoint(&next))) {
      out.append(reinterpret_cast<const char*>(p), next - p);
      p = next;
      continue;
    }
    const unsigned char* runStart = p;
    p = next;
    while (*p) {
      next = p;
      if (!isRunCodepoint(utf8NextCodepoint(&next))) break;
      p = next;
    }
    if (!appendShapedRun(reinterpret_cast<const char*>(runStart), p - runStart, out)) return false;
    shapedAny = true;
  }
  return shapedAny;
}

void ComplexShaper::releaseLocked() {
  // Cached runs stay valid: the same source and scale shape them identically
  // after a rebuild. They are forgotten when either changes (or on destruction).
  if (buffer_ != nullptr) hb_buffer_destroy(buffer_);
  if (font_ != nullptr) hb_font_destroy(font_);
  if (face_ != nullptr) dropFace(face_);
  buffer_ = nullptr;
  font_ = nullptr;
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

void ComplexShaper::deallocate(void* ptr) {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  budgetedFree(ptr);
}

ComplexShaper::MemoryStats ComplexShaper::memoryStats() {
  std::lock_guard<std::recursive_mutex> lock(shaperMutex());
  return MemoryStats{gCurrent, gPeak, gBudget, gFailures, gShapedRuns, gReusedRuns};
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
  const size_t before = gCurrent;
  for (ComplexShaper* shaper = gShapers; shaper != nullptr; shaper = shaper->next_) shaper->releaseLocked();
  releaseCache();
  memoFree();  // later runs in the scope re-shape; nothing reads a freed entry
  return before - gCurrent;
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
