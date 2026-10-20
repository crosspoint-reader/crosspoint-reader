#include "TtfFont.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

using glyphstore::kIdentityBytes;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

TtfFont::Hooks hooks;

// TtfFontSet::storeEvents(): written under RenderLock, read from any task.
std::atomic<uint32_t> storeFills{0};
std::atomic<uint32_t> storeWipes{0};
std::atomic<uint32_t> lastWipeMs{0};

// What a default-ignorable codepoint draws as: no advance and no ink, as the converter makes them (spec 8.1).
const EpdGlyph kIgnorable{};

constexpr uint32_t kCopyBytes = 4096;  // install: one SD read and one flash write per chunk
constexpr uint8_t kMaxFailuresLogged = 16;
// An accent family is copied whole (flash mode) only while the log keeps this much, about 5,000 Latin glyphs: an
// accent draws a few headings and words, the log every glyph of the books read until a wipe.
constexpr uint32_t kAccentFlashLogBytes = 8 * glyphstore::kPageBytes;

// What an install holds while it runs: the copy buffer, the SD-mode block being planned or copied, and the SD-mode
// styles' header records. Freed when it ends.
struct InstallWork {
  uint8_t buffer[kCopyBytes];
  ttf::SdBlock block;
  glyphstore::StyleInfo sd[glyphstore::kMaxStyles];
};

// The largest native size whose glyphs all fit EpdGlyphRef's int8 top and left, by the face's bounding box: the
// pixel size is rounded, and a glyph's edges round outwards, so a pixel is kept in hand.
uint8_t nativeCeilingPx(const ttf::Face& face) {
  uint32_t px = TtfFont::kMaxNativePx;
  if (face.unitsPerEm == 0) return static_cast<uint8_t>(px);
  if (face.yMax > 0) px = std::min<uint32_t>(px, INT8_MAX * static_cast<uint32_t>(face.unitsPerEm) / face.yMax - 1);
  if (face.xMin < 0) {
    px = std::min<uint32_t>(px, -INT8_MIN * static_cast<uint32_t>(face.unitsPerEm) / -face.xMin - 1);
  }
  return static_cast<uint8_t>(px);
}

// EpdGlyphRef carries left and top as int8 (spec 8.1): a glyph reaching further from the pen cannot be placed.
ttf::Status placeable(const ttf::Status loaded, const EpdGlyph& metrics) {
  if (loaded != ttf::Status::Ok) return loaded;
  const bool fits =
      metrics.left >= INT8_MIN && metrics.left <= INT8_MAX && metrics.top >= INT8_MIN && metrics.top <= INT8_MAX;
  return fits ? loaded : ttf::Status::Unsupported;
}

void checkLock() {
  static bool logged = false;
  if (hooks.renderLockHeld && !logged && !hooks.renderLockHeld()) {
    logged = true;
    LOG_ERR("TTF", "glyph rendered without RenderLock (spec 3.4)");
  }
}

// Whether the flash at `offset` holds `file` already, compared in halves of the install's copy buffer (the file's
// beside the flash's); a block that differs stops at its first chunk that does.
bool holdsFile(const NorFlash& flash, const uint32_t offset, const ttf::FileSource& file, uint8_t* buffer) {
  constexpr uint32_t kHalf = kCopyBytes / 2;
  for (uint32_t at = 0; at < file.size; at += kHalf) {
    const uint32_t len = std::min(kHalf, file.size - at);
    if (!file.read(file.ctx, at, buffer, len) || !flash.read(offset + at, buffer + kHalf, len) ||
        std::memcmp(buffer, buffer + kHalf, len) != 0) {
      return false;
    }
    if (hooks.yield) hooks.yield();
  }
  return true;
}

}  // namespace

void TtfFont::setHooks(const Hooks& newHooks) { hooks = newHooks; }

// --- The set: install, open, wipe ---

TtfFontSet::TtfFontSet(NorFlash& flash) : flash_(flash), store_(flash) {}

bool TtfFontSet::load(const Files* families, const uint8_t count, const uint8_t identity[kIdentityBytes],
                      const std::function<void()>& onBusy, const Install mode) {
  count_ = 0;
  if (count == 0 || count > kMaxFamilies) return false;
  for (uint8_t f = 0; f < count; ++f) {
    if (families[f][0].size == 0) {
      LOG_ERR("TTF", "family %u has no regular style", f);
      return false;
    }
    fonts_[f].reset(new (std::nothrow) TtfFont(*this, f));
    if (!fonts_[f]) {
      LOG_ERR("TTF", "OOM: family %u", f);
      return false;
    }
  }
  bool installed = false;
  OpenResult opened = store_.open(identity);
  if (opened == OpenResult::NotInstalled || opened == OpenResult::OtherFamily || opened == OpenResult::MapFailed) {
    if (mode == Install::Never) {
      LOG_INF("TTF", "the store does not hold the set (%u); not installed here", static_cast<unsigned>(opened));
      return false;
    }
    if (!install(families, count, identity, onBusy)) return false;
    installed = true;
    opened = store_.open(identity);
  }
  if (opened != OpenResult::Ready) {
    LOG_ERR("TTF", "glyph store did not open (%u)", static_cast<unsigned>(opened));
    return false;
  }

  ttf::ScratchPlan plan;
  TtfFont::StylesResult styles = openStyles(families, count, &plan);
  if ((styles == TtfFont::StylesResult::MapFailed || styles == TtfFont::StylesResult::Changed) && !installed &&
      mode == Install::IfNeeded) {
    // The regions opened but a style no longer maps, as the mapping budget shrank since the install (a firmware
    // update), or an SD-mode style's file is not the one installed. Install again, with a plan for today's budget
    // and today's files (spec 6.2); a fresh install that still cannot open its styles is refused below rather than
    // installed again at every load.
    LOG_INF("TTF", "%s; installing the families again",
            styles == TtfFont::StylesResult::MapFailed ? "a style no longer fits the mapping budget"
                                                       : "a style's file on the card changed since the install");
    if (!install(families, count, identity, onBusy)) return false;
    opened = store_.open(identity);
    if (opened != OpenResult::Ready) {
      LOG_ERR("TTF", "glyph store did not open after a reinstall (%u)", static_cast<unsigned>(opened));
      return false;
    }
    plan = ttf::ScratchPlan{};
    styles = openStyles(families, count, &plan);
  }
  if (styles != TtfFont::StylesResult::Ok) return false;

  // One block for every family: the output region doubles as the batch buffer, so it also holds the record header
  // of its largest bitmap. Planned only: the first miss makes it (beginMiss()).
  plan.outputBytes = std::min(plan.outputBytes + glyphstore::kRecordHeaderBytes + 3, ttf::kMaxOutputBytes);
  releaseMissMemory();
  if (!rasterizer_.setPlan(plan)) return false;
  count_ = count;
  readsCard_ = false;
  sdPageReads_ = 0;
  for (uint8_t f = 0; f < count; ++f) readsCard_ = readsCard_ || fonts_[f]->readsCard();
  if (store_.wipePending() && mode == Install::IfNeeded) wipeIfPending(onBusy);
  LOG_INF("TTF",
          "%u famil%s loaded: %u B scratch, %u B SD pages (made on a miss), %u B per native size and style, log %lu of "
          "%lu B used",
          count, count == 1 ? "y" : "ies", static_cast<unsigned>(rasterizer_.scratchBytes()),
          static_cast<unsigned>(readsCard_ ? sizeof(ttf::SdPageCache) : 0),
          static_cast<unsigned>(sizeof(TtfFont::Size::StyleFont)), static_cast<unsigned long>(store_.logUsed()),
          static_cast<unsigned long>(store_.geometry().logSize));
  for (uint8_t f = 0; f < count; ++f) {
    LOG_INF("TTF", "family %u regular style: %u kern subtables%s, %u ligatures", f,
            fonts_[f]->styles_[0].kerning.subtableCount(),
            fonts_[f]->styles_[0].kerning.legacy() ? " (legacy kern table)" : "",
            fonts_[f]->styles_[0].ligatures.count);
  }
  return true;
}

TtfFont::StylesResult TtfFontSet::openStyles(const Files* families, const uint8_t count, ttf::ScratchPlan* plan) {
  // Every family's present styles take the store's style blocks in the set's order, at install as here.
  uint8_t block = 0;
  for (uint8_t f = 0; f < count; ++f) {
    const TtfFont::StylesResult result = fonts_[f]->openStyles(families[f], block, plan);
    if (result != TtfFont::StylesResult::Ok) return result;
  }
  return TtfFont::StylesResult::Ok;
}

bool TtfFontSet::install(const Files* families, const uint8_t count, const uint8_t* identity,
                         const std::function<void()>& onBusy) {
  store_.close();  // the plan needs the free mapping pages with nothing of the store mapped
  // Every family's present styles, in the set's order: the store's style blocks.
  const ttf::FileSource* files[glyphstore::kMaxStyles] = {};
  uint8_t familyOf[glyphstore::kMaxStyles] = {};
  uint8_t styles = 0;
  for (uint8_t f = 0; f < count; ++f) {
    for (const ttf::FileSource& file : families[f]) {
      if (file.size == 0) continue;
      if (styles == glyphstore::kMaxStyles) {
        LOG_ERR("TTF", "the families have more than %u styles", glyphstore::kMaxStyles);
        return false;
      }
      files[styles] = &file;
      familyOf[styles++] = f;
    }
  }
  const auto work = makeUniqueNoThrow<InstallWork>();
  if (!work) {
    LOG_ERR("TTF", "install: no %u B to work in", static_cast<unsigned>(sizeof(InstallWork)));
    return false;
  }

  // Flash mode while the whole set fits; past that the families go to SD mode from the last on, the least used
  // accent first and the body last of all (from family `sdFrom` on, every one is in SD mode). An accent family keeps
  // flash mode only while the log keeps kAccentFlashLogBytes. A style whose SD-mode block cannot be made keeps size
  // 0 there, which the planner refuses.
  glyphstore::PlanInput in{};
  in.styleCount = styles;
  in.usableBytes = flash_.size();
  in.freeMapPages = flash_.freeMapPages();
  glyphstore::Layout layout{};
  glyphstore::Layout fits{};  // the first plan that fits, should none keep the log the rule asks for
  bool fitted = false;
  bool planned = false;
  for (int sdFrom = count; sdFrom >= 0 && !planned; --sdFrom) {
    for (uint8_t b = 0; b < styles; ++b) {
      glyphstore::StyleInfo& sd = work->sd[b];
      if (sdFrom == count - 1 && work->block.plan(*files[b])) {
        // The SD-mode blocks, planned once from each file's table directory, when first wanted.
        sd.size = work->block.bytes();
        sd.fileStamp = files[b]->stamp;
        sd.fileSize = files[b]->size;
        sd.outlines = work->block.outlines();
      }
      const bool card = familyOf[b] >= sdFrom;
      in.styleMode[b] = card ? glyphstore::kModeSd : glyphstore::kModeFlash;
      in.styleBytes[b] = card ? sd.size : files[b]->size;
    }
    if (!glyphstore::planLayout(in, layout)) continue;
    planned = sdFrom <= 1 || layout.geometry.logSize >= kAccentFlashLogBytes;
    if (!fitted) fits = layout;
    fitted = true;
  }
  if (!planned && fitted) {
    layout = fits;  // nothing with more on the card fits: whole, with a smaller log, beats a family left out
    planned = true;
  }
  if (!planned) {
    // Not an error yet: the manager tries again without the last accent family, and logs if even the body cannot.
    LOG_INF("TTF", "families do not fit the font partition (%u styles, %lu free mapping pages)", styles,
            static_cast<unsigned long>(in.freeMapPages));
    return false;
  }
  if (onBusy) onBusy();

  // A block's bytes as a file: the style's own file in flash mode, else its SD-mode block made from it.
  const auto blockSource = [&](const uint8_t b, ttf::FileSource* source) {
    if (layout.styleMode[b] == glyphstore::kModeFlash) {
      *source = *files[b];
      return true;
    }
    if (!work->block.plan(*files[b])) return false;
    *source = work->block.source();
    return true;
  };
  // A style block that holds its bytes already where this layout puts it stays as it is, neither erased nor copied:
  // a set that keeps its leading families (a TrueType reader font beside another book's accent families, or beside
  // none) copies only the rest. Reading a kept block costs a fraction of erasing and writing it again.
  uint8_t kept = 0;
  unsigned onCard = 0;
  for (uint8_t b = 0; b < styles; ++b) {
    ttf::FileSource source;
    if (blockSource(b, &source) && holdsFile(flash_, layout.styleOffset[b], source, work->buffer)) {
      kept |= static_cast<uint8_t>(1u << b);
    }
    onCard += layout.styleMode[b] == glyphstore::kModeSd ? 1 : 0;
  }
  if (!store_.beginInstall(layout, identity, work->sd, kept)) return false;
  for (uint8_t b = 0; b < styles; ++b) {
    if ((kept & (1u << b)) != 0) continue;
    ttf::FileSource source;
    bool copied = blockSource(b, &source);
    for (uint32_t offset = 0; copied && offset < source.size; offset += kCopyBytes) {
      const uint32_t len = std::min(kCopyBytes, source.size - offset);
      copied =
          source.read(source.ctx, offset, work->buffer, len) && store_.writeStyleData(b, offset, work->buffer, len);
      if (hooks.yield) hooks.yield();
    }
    if (!copied) {
      LOG_ERR("TTF", "install: copying family %u's style block %u failed", familyOf[b], b);
      return false;  // the header is still erased: the next load installs again
    }
  }
  if (!store_.finishInstall()) return false;
  LOG_INF("TTF", "%u famil%s installed: %u styles (%d kept, %u in SD mode), log %lu B", count, count == 1 ? "y" : "ies",
          styles, __builtin_popcount(kept), onCard, static_cast<unsigned long>(layout.geometry.logSize));
  return true;
}

bool TtfFontSet::wipeIfPending(const std::function<void()>& onBusy) {
  if (!store_.isOpen() || !store_.wipePending()) return false;
  if (onBusy) onBusy();
  const uint32_t start = hooks.nowMs ? hooks.nowMs() : 0;
  if (!store_.wipe()) return false;  // stays pending: the next book open tries again
  lastWipeMs = hooks.nowMs ? hooks.nowMs() - start : 0;
  ++storeWipes;
  LOG_INF("TTF", "glyph store wiped");
  ringMode_ = false;
  freeRing();
  // The log may map elsewhere now; every size claims its index again at its next lookup.
  for (uint8_t f = 0; f < count_; ++f) {
    TtfFont& font = *fonts_[f];
    font.rebase(font.body_);
    for (auto& size : font.native_) {
      if (size) font.rebase(*size);
    }
  }
  return true;
}

bool TtfFontSet::markFull() {
  if (!store_.isOpen()) return false;
  store_.markFull();
  noteCommit(Commit::Full);
  return true;
}

TtfFontSet::Stats TtfFontSet::stats() const {
  Stats s;
  s.families = count_;
  s.readsCard = readsCard_;
  s.ringMode = ringMode_;
  s.wipePending = store_.isOpen() && store_.wipePending();
  s.logUsed = store_.logUsed();
  s.logSize = store_.isOpen() ? store_.geometry().logSize : 0;
  s.usedSlots = store_.usedSlots();
  for (uint8_t f = 0; f < count_; ++f) {
    s.renders += fonts_[f]->renders();
    s.hits += fonts_[f]->storeHits();
  }
  return s;
}

TtfFontSet::StoreEvents TtfFontSet::storeEvents() { return {storeFills.load(), storeWipes.load(), lastWipeMs.load()}; }

// --- The set: what only a miss needs ---

// The memory one render() or prefetch() draws its glyphs in: made (or found) before the first outline is read, and,
// when it came from the lent arena, given back as the call returns.
class TtfFontSet::MissScope {
 public:
  explicit MissScope(TtfFontSet& set) : set_(set), ready_(set.beginMiss()) {}
  ~MissScope() { set_.endMiss(); }
  MissScope(const MissScope&) = delete;
  MissScope& operator=(const MissScope&) = delete;
  bool ready() const { return ready_; }

 private:
  TtfFontSet& set_;
  bool ready_ = false;
};

bool TtfFontSet::beginMiss() {
  if (rasterizer_.allocated() && (!readsCard_ || sdPages_)) return true;  // on the heap since an earlier miss
  BuildArena* const arena = glyph_miss::arena();
  // Room for all of it, the worst padding included: a refused allocation would read in the arena's own log as its
  // owner running out.
  const size_t want = rasterizer_.scratchBytes() + alignof(std::max_align_t) +
                      (readsCard_ ? sizeof(ttf::SdPageCache) + alignof(ttf::SdPageCache) : 0);
  if (arena && !rasterizer_.allocated() && !sdPages_ && arena->capacity() - arena->used() >= want) {
    // All of it from the lent region, for this lookup alone: endMiss() gives it back, so the arena's owner finds its
    // blocks as it left them.
    lent_ = arena->reserveBlock();
    auto* const scratch = static_cast<uint8_t*>(arena->alloc(rasterizer_.scratchBytes()));
    void* const pages =
        scratch && readsCard_ ? arena->alloc(sizeof(ttf::SdPageCache), alignof(ttf::SdPageCache)) : nullptr;
    if (scratch && (pages || !readsCard_)) {
      rasterizer_.attach(scratch);
      if (pages) usePages(new (pages) ttf::SdPageCache());
      lentFrom_ = arena;
      return true;
    }
    arena->release(lent_);  // no room left in it: the heap then
  }
  // On the heap, until releaseMissMemory(). When the heap cannot hold it, what the app can spare goes first, once;
  // after that the misses of the pass go without at once, rather than ask a heap that just said no for each glyph.
  for (bool reclaimed = false; !missesStarved_; reclaimed = true) {
    if (rasterizer_.allocate() && (!readsCard_ || sdPages_ || heapPages())) return true;
    missesStarved_ = reclaimed || !hooks.reclaim || !hooks.reclaim();
  }
  return false;
}

void TtfFontSet::endMiss() {
  if (!lentFrom_) return;
  rasterizer_.release();
  if (sdPages_) sdPageReads_ += sdPages_->misses();
  usePages(nullptr);
  lentFrom_->release(lent_);
  lentFrom_ = nullptr;
}

void TtfFontSet::noMemory(const uint32_t codepoint) {
  (void)codepoint;  // only logged, and logging may be compiled out
  glyph_miss::noteStarved();
  if (failuresLogged_ < kMaxFailuresLogged) {
    ++failuresLogged_;
    LOG_ERR("TTF", "U+%04lX: no %lu B to draw it in, drawn as missing", static_cast<unsigned long>(codepoint),
            static_cast<unsigned long>(rasterizer_.scratchBytes() + (readsCard_ ? sizeof(ttf::SdPageCache) : 0)));
  }
}

bool TtfFontSet::heapPages() {
  ownedPages_ = makeUniqueNoThrow<ttf::SdPageCache>();
  usePages(ownedPages_.get());
  return sdPages_ != nullptr;
}

void TtfFontSet::usePages(ttf::SdPageCache* pages) {
  // Every SD-mode style reads its outlines through it, a FontBytes made before it was too (fontBytes()).
  sdPages_ = pages;
  for (uint8_t f = 0; f < count_; ++f) {
    TtfFont& font = *fonts_[f];
    for (uint8_t i = 0; i < font.styleCount_; ++i) font.styles_[i].sd.cache = pages;
  }
}

uint32_t TtfFontSet::missMemoryBytes() const {
  return (rasterizer_.allocated() ? rasterizer_.scratchBytes() : 0) +
         (ownedPages_ ? static_cast<uint32_t>(sizeof(ttf::SdPageCache)) : 0) + ringBytes();
}

uint32_t TtfFontSet::releaseMissMemory() {
  missesStarved_ = false;                                                      // the next pass asks the heap again
  if (!rasterizer_.allocated() && !ownedPages_ && ringBytes() == 0) return 0;  // as RenderLock goes back, mostly
  const uint32_t freed = missMemoryBytes();
  rasterizer_.release();
  if (sdPages_) sdPageReads_ += sdPages_->misses();
  usePages(nullptr);
  ownedPages_.reset();
  freeRing();
  return freed;
}

// --- The set: the ring, and what a failed write means ---

const EpdGlyph* TtfFontSet::drawToRing(const EpdGlyph& metrics) {
  const uint8_t slot = ringNext_;
  ringNext_ = static_cast<uint8_t>((ringNext_ + 1) % kRingSlots);
  if (metrics.dataLength > ringCapacity_[slot]) {
    // One heap block per slot, kept and reused while it is large enough, until the pass ends (releaseMissMemory()):
    // the ring exists only after the store filled up, for oversized glyphs, for a size the store had no room for, or
    // without a store. A record is promised only until the next miss, so the oldest slots give way when the ring
    // would pass its budget.
    freeRingSlot(slot);
    for (uint8_t age = 1; age < kRingSlots && ringBytes() + metrics.dataLength > kRingBudgetBytes; ++age) {
      freeRingSlot(static_cast<uint8_t>((slot + age) % kRingSlots));  // the slot after this one is the oldest
    }
    ringBitmaps_[slot] = makeUniqueNoThrow<uint8_t[]>(metrics.dataLength);
    ringCapacity_[slot] = ringBitmaps_[slot] ? metrics.dataLength : 0;
    if (!ringBitmaps_[slot]) {
      LOG_ERR("TTF", "ring: no %u B for a glyph", static_cast<unsigned>(metrics.dataLength));
      return nullptr;
    }
  }
  if (metrics.dataLength > 0 && rasterizer_.rasterize(ringBitmaps_[slot].get()) != ttf::Status::Ok) return nullptr;
  ringGlyphs_[slot] = metrics;
  ringGlyphs_[slot].dataOffset = 0;
  return &ringGlyphs_[slot];
}

void TtfFontSet::freeRingSlot(const uint8_t slot) {
  ringBitmaps_[slot].reset();
  ringCapacity_[slot] = 0;
}

void TtfFontSet::freeRing() {
  for (uint8_t i = 0; i < kRingSlots; ++i) freeRingSlot(i);
}

uint32_t TtfFontSet::ringBytes() const {
  uint32_t bytes = 0;
  for (const uint16_t capacity : ringCapacity_) bytes += capacity;
  return bytes;
}

bool TtfFontSet::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
  // An address-range check: every glyph drawn from the store pays this call.
  const auto at = reinterpret_cast<uintptr_t>(glyph);
  const auto first = reinterpret_cast<uintptr_t>(ringGlyphs_);
  if (at >= first && at < first + sizeof ringGlyphs_) {
    const uint32_t slot = static_cast<uint32_t>((at - first) / sizeof(EpdGlyph));
    bitmap = ringGlyphs_[slot].dataLength > 0 ? ringBitmaps_[slot].get() : nullptr;
    return true;
  }
  if (glyph == &kIgnorable) {
    bitmap = nullptr;
    return true;
  }
  return false;
}

void TtfFontSet::failed(const uint8_t style, const uint32_t codepoint, const ttf::Status status, const bool haveKey,
                        const uint32_t key) {
  (void)style;  // both only logged, and logging may be compiled out
  (void)codepoint;
  // Transient (no memory to draw it in, an SD read failed): drawn as missing this time, asked again at the next lookup.
  if (!ttf::isPermanent(status)) return;
  if (haveKey && !ringMode_) noteCommit(store_.markNoGlyph(key));
  if (failuresLogged_ < kMaxFailuresLogged) {
    ++failuresLogged_;
    LOG_INF("TTF", "U+%04lX style %u: %s, drawn as missing", static_cast<unsigned long>(codepoint), style,
            ttf::statusName(status));
  }
}

void TtfFontSet::noteCommit(const Commit result) {
  if (result == Commit::Full || result == Commit::WriteFailed) {
    if (!ringMode_) {
      LOG_INF("TTF", "glyph store %s: new glyphs are not stored until the next book open",
              result == Commit::Full ? "full" : "write failed");
      ++storeFills;
    }
    ringMode_ = true;
  } else if (result == Commit::Refused) {
    LOG_ERR("TTF", "glyph store refused a write");
  }
}

// --- A family: its styles and sizes ---

TtfFont::~TtfFont() = default;  // out of line: its styles and sizes are freed in one place, not in every set's

TtfFont::StylesResult TtfFont::openStyles(const ttf::FileSource (&files)[4], uint8_t& block, ttf::ScratchPlan* plan) {
  maxNativePx_ = kMaxNativePx;
  if (!styles_) {
    // The styles the family has files for, and the body size's fonts of them. Opened again after an install, the
    // family keeps them: its files are the same.
    uint8_t count = 0;
    for (uint8_t s = 0; s < 4; ++s) slot_[s] = files[s].size != 0 ? count++ : kNoSlot;
    styles_ = makeUniqueNoThrow<Style[]>(count);
    body_.styles = makeUniqueNoThrow<Size::StyleFont[]>(count);
    if (!styles_ || !body_.styles) {
      LOG_ERR("TTF", "OOM: family %u's %u styles", family_, count);
      return StylesResult::Failed;
    }
    styleCount_ = count;
  }
  for (uint8_t s = 0; s < 4; ++s) {
    if (files[s].size == 0) continue;
    Style& style = styles_[slot_[s]];
    style.present = false;
    style.onCard = false;
    style.sd = {};
    style.face = {};
    style.kerning = ttf::Kerning{};
    style.ligatures = ttf::Ligatures{};
    std::fill_n(style.uncoveredLeft, sizeof style.uncoveredLeft, 0);
    style.block = block++;
    glyphstore::StyleInfo info{};
    if (!set_.store_.styleInfo(style.block, info)) return StylesResult::Failed;
    style.bytes = info.size;
    if (info.mode == glyphstore::kModeSd) {
      // The outlines are read from the file the block was made from, or from none: another file's would not be where
      // its loca says (spec 6.2).
      if (files[s].stamp != info.fileStamp || files[s].size != info.fileSize) return StylesResult::Changed;
      style.onCard = true;
      style.sd = {files[s], info.outlines.fontOffset, info.outlines.fileOffset, info.outlines.length, nullptr};
    }
    FontBytes bytes;
    if (!fontBytes(s, &bytes)) return StylesResult::MapFailed;
    if (style.onCard) {
      // The stamp misses a file rewritten in place at the same size and time; its table directory does not.
      auto block = makeUniqueNoThrow<ttf::SdBlock>();
      if (!block) return StylesResult::Failed;
      if (!block->plan(files[s]) || !block->sameAs(bytes.span(0, style.bytes), style.bytes) ||
          block->outlines().fileOffset != style.sd.fileOffset) {
        return StylesResult::Changed;
      }
    }
    if (ttf::parseFace(bytes, &style.face) != ttf::FaceStatus::Ok) {
      LOG_ERR("TTF", "family %u style %u is not a font the engine draws", family_, s);
      continue;
    }
    style.present = true;
    // Kerning and ligatures, resolved once per style (spec 5.4): a style without them draws without.
    style.kerning.resolve(bytes, style.face);
    ttf::resolveLigatures(bytes, style.face, &style.ligatures);
    maxNativePx_ = std::min(maxNativePx_, nativeCeilingPx(style.face));
    // Planned for the largest size drawn: a native size (5 x 26 pt's pixels per em, about) above the body sizes. A
    // body too large for flash mode is a CJK font in practice, and draws about 37% faster with the larger pool.
    const uint32_t largest = std::max(ttf::ppemFromPoints(kMaxPointSize), static_cast<uint32_t>(kMaxNativePx) << 6);
    const uint32_t pool = style.onCard && family_ == 0 ? ttf::kPoolBytesSd : ttf::kPoolBytesFlash;
    *plan = ttf::mergePlans(*plan, ttf::planScratch(style.face, largest, pool));
  }
  if (!draws(0)) return StylesResult::Failed;
  return StylesResult::Ok;
}

bool TtfFont::configure(Size& size, const uint32_t ppem) {
  ttf::LineMetrics metrics[4];
  for (uint8_t s = 0; s < 4; ++s) {
    if (!draws(s)) continue;
    metrics[s] = ttf::lineMetrics(styleAt(s)->face, ppem);
    if (metrics[s].advanceY <= 0 || metrics[s].advanceY > 255) {
      LOG_ERR("TTF", "style %u at %lu/64 px: line height %ld does not fit", s, static_cast<unsigned long>(ppem),
              static_cast<long>(metrics[s].advanceY));
      return false;
    }
  }
  size.ppem = ppem;
  size.claimTried = false;
  for (uint8_t s = 0; s < 4; ++s) {
    if (slot_[s] == kNoSlot) continue;
    Size::StyleFont& style = size.styles[slot_[s]];
    style.provider.owner = this;
    style.provider.size = &size;
    style.provider.style = s;
    style.data = EpdFontData{};
    if (!draws(s)) continue;
    style.data.advanceY = static_cast<uint8_t>(metrics[s].advanceY);
    style.data.ascender = metrics[s].ascender;
    style.data.descender = metrics[s].descender;
    style.data.is2Bit = true;
    style.data.bitmap = set_.store_.logBase();
    style.data.provider = &style.provider;
    const ttf::Ligatures& ligatures = styleAt(s)->ligatures;
    style.data.ligaturePairs = ligatures.count != 0 ? ligatures.pairs : nullptr;
    style.data.ligaturePairCount = ligatures.count;
  }
  return true;
}

void TtfFont::rebase(Size& size) {
  if (size.ppem == 0) return;
  for (uint8_t i = 0; i < styleCount_; ++i) {
    if (styles_[i].present) size.styles[i].data.bitmap = set_.store_.logBase();
  }
}

bool TtfFont::setBodySize(const uint8_t pointSize) {
  if (pointSize == 0 || pointSize > kMaxPointSize || !draws(0)) return false;
  if (!configure(body_, ttf::ppemFromPoints(pointSize))) return false;
  pointSize_ = pointSize;
  claimSize(body_);
  return true;
}

const EpdFont* TtfFont::font(const uint8_t style) const {
  return draws(style) && body_.ppem != 0 ? &body_.styles[slot_[style]].font : nullptr;
}

bool TtfFont::ringMode() const { return set_.ringMode(); }

bool TtfFont::readsCard() const {
  for (uint8_t i = 0; i < styleCount_; ++i) {
    if (styles_[i].present && styles_[i].onCard) return true;
  }
  return false;
}

TtfFont::Size* TtfFont::nativeSize(const uint8_t px) {
  if (px < kMinNativePx || px > maxNativePx_ || !draws(0)) return nullptr;
  const uint32_t ppem = static_cast<uint32_t>(px) << 6;
  std::unique_ptr<Size>* free = nullptr;
  for (auto& size : native_) {
    if (size && size->ppem == ppem) return size.get();
    if (!size && !free) free = &size;
  }
  if (!free) {
    LOG_ERR("TTF", "%u px: all %u native sizes in use; the nearest serves", px, kMaxNativeSizes);
    return nearestNative(ppem);
  }
  checkLock();
  auto size = makeUniqueNoThrow<Size>();
  if (size) size->styles = makeUniqueNoThrow<Size::StyleFont[]>(styleCount_);
  if (!size || !size->styles) {
    LOG_ERR("TTF", "%u px: no memory for a native size; the nearest serves", px);
    return nearestNative(ppem);
  }
  if (!configure(*size, ppem)) return nullptr;
  *free = std::move(size);
  return free->get();
}

TtfFont::Size* TtfFont::nearestNative(const uint32_t ppem) {
  Size* nearest = nullptr;
  uint32_t distance = UINT32_MAX;
  for (auto& size : native_) {
    if (!size) continue;
    const uint32_t d = size->ppem > ppem ? size->ppem - ppem : ppem - size->ppem;
    if (d < distance) {
      distance = d;
      nearest = size.get();
    }
  }
  return nearest;
}

bool TtfFont::nativeFonts(const uint8_t px, const EpdFont* (&fonts)[4], uint32_t* servedPpem) {
  Size* size = nativeSize(px);
  if (!size) return false;
  for (uint8_t s = 0; s < 4; ++s) fonts[s] = draws(s) ? &size->styles[slot_[s]].font : nullptr;
  if (servedPpem) *servedPpem = size->ppem;
  return true;
}

void TtfFont::dropNativeSizes() {
  for (auto& size : native_) size.reset();
  // No record is in use either (the same safe point): the ring's blocks, which a native size grew, go too.
  set_.freeRing();
}

uint8_t TtfFont::nativeSizeCount() const {
  uint8_t count = 0;
  for (const auto& size : native_) count += size ? 1 : 0;
  return count;
}

// --- A family: its glyphs ---

bool TtfFont::claimSize(Size& size) {
  // A wipe voids every face index: claimed again once the store's generation moved. A claim the full table
  // refused is not asked again before then; that size's glyphs come from the ring.
  FlashGlyphStore& store = set_.store_;
  if (size.claimTried && size.generation == store.generation()) return size.indexValid;
  size.claimTried = true;
  size.generation = store.generation();
  size.indexValid = store.faceIndex(family_, size.ppem, &size.index);
  return size.indexValid;
}

bool TtfFont::storeKey(Size& size, const uint8_t style, const uint32_t codepoint, uint32_t* key) {
  if (!set_.store_.isOpen() || size.ppem == 0 || !claimSize(size)) return false;
  *key = glyphstore::makeKey(codepoint, style, size.index);
  return true;
}

bool TtfFont::fontBytes(const uint8_t style, FontBytes* bytes) {
  // Flash mode maps one style block at a time, every family's, so the pointer is fetched again for every render.
  Style& s = *styleAt(style);
  const uint8_t* mapped = set_.store_.mapStyle(s.block);
  if (!mapped) {
    LOG_ERR("TTF", "family %u style %u could not be mapped", family_, style);
    return false;
  }
  s.sd.cache = set_.sdPages_;  // the set's while a miss has one (usePages())
  *bytes = s.onCard ? FontBytes(mapped, s.bytes, &s.sd) : FontBytes(mapped, s.bytes);
  return true;
}

const EpdGlyph* TtfFont::glyph(Size& size, const uint8_t style, const uint32_t codepoint) {
  if (!draws(style) || size.ppem == 0) return nullptr;
  if (utf8IsDefaultIgnorable(codepoint)) return &kIgnorable;
  uint32_t key = 0;
  const bool haveKey = storeKey(size, style, codepoint, &key);
  if (haveKey) {
    const EpdGlyph* stored = nullptr;
    switch (set_.store_.find(key, &stored)) {
      case Lookup::Found:
        ++hits_;
        return stored;
      case Lookup::NoGlyph:
        return nullptr;
      case Lookup::NotStored:
        break;
    }
  }
  return render(size, style, codepoint, haveKey, key);
}

const EpdGlyph* TtfFont::render(const Size& size, const uint8_t style, const uint32_t codepoint, const bool haveKey,
                                const uint32_t key) {
  checkLock();
  FontBytes bytes;
  if (!fontBytes(style, &bytes)) return nullptr;
  TtfFontSet& set = set_;
  const Style& drawn = *styleAt(style);
  const ttf::Face& face = drawn.face;
  const uint16_t index = drawn.ligatures.glyphFor(bytes, face, codepoint);
  if (index == 0) {
    // The font has no glyph for it. An I/O error is not an answer (SD mode): ask again next time.
    if (haveKey && !set.ringMode_ && !bytes.ioError()) set.noteCommit(set.store_.markNoGlyph(key));
    return nullptr;
  }
  // A glyph to draw: in what the set's first miss makes (spec 5.3).
  const TtfFontSet::MissScope miss(set);
  if (!miss.ready()) {
    set.noMemory(codepoint);
    return nullptr;
  }
  glyph_miss::noteDrawn();
  EpdGlyph metrics{};
  const ttf::Status loaded = placeable(set.rasterizer_.load(bytes, face, index, size.ppem, &metrics), metrics);
  if (loaded != ttf::Status::Ok) {
    set.failed(style, codepoint, loaded, haveKey, key);
    return nullptr;
  }
  ++renders_;
  if (haveKey && !set.ringMode_ && glyphstore::recordBytes(metrics.dataLength) <= set.batchCapacity()) {
    // Drawn straight into a one-record batch in the output region, then stored.
    GlyphBatch batch(set.rasterizer_.output(), set.batchCapacity());
    uint8_t* bitmap = batch.add(key, metrics);
    if (bitmap) {
      const ttf::Status drawn = set.rasterizer_.rasterize(bitmap);
      if (drawn != ttf::Status::Ok) {
        batch.dropLast();
        set.failed(style, codepoint, drawn, haveKey, key);
        return nullptr;
      }
      const Commit committed = set.store_.commit(batch);
      const EpdGlyph* stored = nullptr;
      if (committed == Commit::Stored && set.store_.find(key, &stored) == Lookup::Found) return stored;
      set.noteCommit(committed);
    }
  }
  return set.drawToRing(metrics);
}

int8_t TtfFont::kerning(const Size& size, const uint8_t style, const uint32_t left, const uint32_t right) {
  Style* const found = styleAt(style);
  if (!found || !found->present || !found->kerning.any() || size.ppem == 0) return 0;
  Style& s = *found;
  FlashGlyphStore& store = set_.store_;
  // The store keys a pair by its codepoints (BMP only) and keeps font units, so every size shares it.
  const bool storable = left <= 0xFFFF && right <= 0xFFFF;
  int16_t units = 0;
  if (!storable || !store.isOpen() ||
      !store.findPair(static_cast<uint16_t>(left), static_cast<uint16_t>(right), family_, style, &units)) {
    if (s.knownUncovered(left)) return 0;  // without touching the font: no remap of another style
    checkLock();
    FontBytes bytes;
    if (!fontBytes(style, &bytes)) return 0;
    const uint16_t l = s.ligatures.glyphFor(bytes, s.face, left);
    const uint16_t r = s.ligatures.glyphFor(bytes, s.face, right);
    const bool looked = l != 0 && r != 0;
    bool covered = false;
    const int32_t value = looked ? s.kerning.pair(bytes, s.face, l, r, &covered) : 0;
    ++kernLookups_;
    if (bytes.ioError()) return 0;  // SD mode: not an answer, ask again
    units = static_cast<int16_t>(value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value);
    // A left glyph that kerns with nothing costs no slot (spec 8.1): the CJK repertoire would fill the index.
    if (!covered) {
      if (looked) s.markUncovered(left);  // whatever the right glyph: coverage depends on the left one only
      return 0;
    }
    if (storable && !set_.ringMode_) {
      store.storePair(static_cast<uint16_t>(left), static_cast<uint16_t>(right), family_, style, units);
    }
  }
  // Font units to 4.4 pixels at this size, rounded, clamped like the converter's kern matrix.
  const int64_t num = static_cast<int64_t>(units) * size.ppem;  // 26.6 ppem: units * ppem / upem * 16
  const int64_t den = 4 * static_cast<int64_t>(s.face.unitsPerEm);
  const int64_t value = (num >= 0 ? num + den / 2 : num - den / 2) / den;
  return static_cast<int8_t>(value < INT8_MIN ? INT8_MIN : value > INT8_MAX ? INT8_MAX : value);
}

bool TtfFont::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const { return set_.ownsBitmap(glyph, bitmap); }

void TtfFont::prefetch(Size& size, const uint8_t style, const char* utf8Text) {
  TtfFontSet& set = set_;
  if (!utf8Text || !draws(style) || set.ringMode_) return;
  uint32_t key = 0;
  if (!storeKey(size, style, 0, &key)) return;
  checkLock();
  FontBytes bytes;
  if (!fontBytes(style, &bytes)) return;
  const Style& drawn = *styleAt(style);
  const ttf::Face& face = drawn.face;
  FlashGlyphStore& store = set.store_;
  TtfFontSet::Pending* const pending = set.pending_;

  // The text's codepoints that are not stored yet, each once. One family and style a call, so the style block is
  // mapped once for the whole batch however the page mixes families (spec 11.1).
  uint8_t count = 0;
  uint32_t asciiSeen[4] = {};
  const auto* text = reinterpret_cast<const unsigned char*>(utf8Text);
  while (count < TtfFontSet::kPrefetchGlyphs) {
    const uint32_t cp = utf8NextCodepoint(&text);
    if (cp == 0) break;
    if (cp < 0x80) {
      if (asciiSeen[cp >> 5] & (1u << (cp & 31))) continue;
      asciiSeen[cp >> 5] |= 1u << (cp & 31);
    }
    if (utf8IsDefaultIgnorable(cp)) continue;
    key = glyphstore::makeKey(cp, style, size.index);
    if (store.find(key, nullptr) != Lookup::NotStored) continue;
    if (std::any_of(pending, pending + count, [cp](const TtfFontSet::Pending& p) { return p.codepoint == cp; })) {
      continue;
    }
    const uint16_t index = drawn.ligatures.glyphFor(bytes, face, cp);
    if (index == 0) {
      if (!bytes.ioError()) set.noteCommit(store.markNoGlyph(key));
      if (set.ringMode_) return;
      continue;
    }
    pending[count++] = {cp, index};
  }
  if (count == 0) return;
  // Glyphs to draw, in what the set's first miss makes (spec 5.3). Without it the lookups ask again, one by one.
  const TtfFontSet::MissScope miss(set);
  if (!miss.ready()) return;
  glyph_miss::noteDrawn();

  // In glyph order, which is file order: in SD mode the outline reads then move forward through the file.
  std::sort(pending, pending + count,
            [](const TtfFontSet::Pending& a, const TtfFontSet::Pending& b) { return a.glyph < b.glyph; });
  GlyphBatch batch(set.rasterizer_.output(), set.batchCapacity());
  for (uint8_t i = 0; i < count && !set.ringMode_; ++i) {
    const TtfFontSet::Pending& p = pending[i];
    key = glyphstore::makeKey(p.codepoint, style, size.index);
    EpdGlyph metrics{};
    const ttf::Status loaded = placeable(set.rasterizer_.load(bytes, face, p.glyph, size.ppem, &metrics), metrics);
    if (loaded != ttf::Status::Ok) {
      set.failed(style, p.codepoint, loaded, true, key);
      continue;
    }
    if (glyphstore::recordBytes(metrics.dataLength) > set.batchCapacity()) continue;  // drawn from the ring when used
    uint8_t* bitmap = batch.add(key, metrics);
    if (!bitmap) {
      set.noteCommit(store.commit(batch));
      if (set.ringMode_) break;
      bitmap = batch.add(key, metrics);
      if (!bitmap) continue;
    }
    const ttf::Status drawn = set.rasterizer_.rasterize(bitmap);
    if (drawn != ttf::Status::Ok) {
      batch.dropLast();
      set.failed(style, p.codepoint, drawn, true, key);
      continue;
    }
    ++renders_;
    if (hooks.yield) hooks.yield();
  }
  if (!set.ringMode_) set.noteCommit(store.commit(batch));
}
