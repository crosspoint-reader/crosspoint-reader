#include "FontCacheManager.h"

#include <BuildArena.h>
#include <FontDecompressor.h>
#include <HeapFit.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SmallCaps.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace {

// Appends the first `take` bytes of `text` as small caps draw them: each lowercase letter as the
// capital whose glyph it is drawn with (smallCaps::fold), so the prewarm warms that glyph and not
// the lowercase one nobody draws. A fold never lengthens a codepoint's encoding and a sequence
// that does not decode is copied as it is, so this appends at most `take` bytes.
// Based on crosspoint-reader PR #3891 (Justin Mitchell, @itsthisjustin), which folds the
// codepoints its scan records; this scan records text.
void appendSmallCapsFolded(std::string& scan, const char* text, const size_t take) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  const unsigned char* const end = p + take;
  while (p < end && *p != '\0') {
    const unsigned char* const start = p;
    uint32_t cp = utf8NextCodepoint(&p);
    if (p > end) break;  // a sequence the cap cuts in two: leave it out, like the bytes after it
    if (smallCaps::fold(cp)) {
      utf8AppendCodepoint(scan, cp);
    } else {
      scan.append(reinterpret_cast<const char*>(start), static_cast<size_t>(p - start));
    }
  }
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts,
                                   const std::map<int, SdCardFont*>& sdCardFontAliases)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts), sdCardFontAliases_(sdCardFontAliases) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    if (!font) {
      LOG_ERR("FCM", "clearCache: null SdCardFont pointer for fontId=%d", id);
      continue;
    }
    font->clearCache();
  }
}

size_t FontCacheManager::releaseLargeSdPageArenas(const size_t minBytes) {
  size_t released = 0;
  for (auto& [id, font] : sdCardFonts_) {
    if (font) released += font->releasePageArenaIfAtLeast(minBytes);
  }
  return released;
}

void FontCacheManager::setInkOnlyPrewarm(const bool on) {
  for (auto& [id, font] : sdCardFonts_) {
    if (font) font->setInkOnlyPrewarm(on);
  }
}

const EpdFontFamily* FontCacheManager::findFamily(const int fontId) const {
  if (lookup_) return lookup_(lookupCtx_, fontId);
  const auto it = fontMap_.find(fontId);
  return it != fontMap_.end() ? &it->second : nullptr;
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // Deliberately does NOT clear existing page slots: a page that mixes fonts (body +
  // heading) is prewarmed with one call per font, and clearing here would wipe the
  // previous font's slots — the render would then transiently re-decompress a glyph
  // group per body glyph (~10 ms each, multi-second page turns). The batch clear
  // happens once in endScanAndPrewarm() before the per-font loop.

  // SD card font prewarm path (native ID or scaled alias): prewarm all requested styles in one
  // call. An alias MUST be found here: its ID is in fontMap_ too, and the built-in path below
  // would silently skip it (no groups) and leave every glyph to miss the overflow ring.
  auto sdIt = sdCardFonts_.find(fontId);
  bool isSd = sdIt != sdCardFonts_.end();
  if (!isSd) {
    sdIt = sdCardFontAliases_.find(fontId);
    isSd = sdIt != sdCardFontAliases_.end();
  }
  if (isSd) {
    SdCardFont* sdFont = sdIt->second;
    if (!sdFont) {
      LOG_ERR("FCM", "prewarmCache(SD): null SdCardFont pointer for fontId=%d", fontId);
      return;
    }
    int missed = sdFont->prewarm(utf8Text, styleMask);
    if (missed < 0) {
      LOG_ERR("FCM", "prewarmCache(SD): prewarm failed for fontId=%d (styleMask=0x%02X)", fontId, styleMask);
    } else if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // A face whose glyphs come from a provider (TTF): each requested style renders its missing glyphs in
  // one batch. Nothing for built-in fonts.
  const EpdFontFamily* family = findFamily(fontId);
  if (family) family->prefetch(utf8Text, styleMask);

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || !family) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = family->getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed < 0) {
      LOG_DBG("FCM", "prewarmCache: Decompressor slots full during style %d!", i);
    } else if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    if (font) font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    if (font) font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  // Bounded: this is every drawn string of a page, concatenated per (font, style), and it used
  // to grow by doubling for as long as the page had text (audit §4.4). A page's distinct glyphs
  // fit in far less than this; text past the cap is simply not prewarmed and loads on demand.
  constexpr size_t SCAN_BYTES_PER_STYLE = 4096;
  // Grown in steps, not reserved whole. A page's text is a few KB in its body style and a few
  // words in each of the others, and every (font, style) on the page used to reserve the full cap
  // up front: four styles held 16 KB until the prewarm ended, on top of the prewarm's own buffers.
  // That was most of a pre-render's cost on the X3 (2026-10-01: a pass costing 26.9 KB, ~17 KB of
  // it transient). The cap still bounds the body style.
  constexpr size_t SCAN_GROWTH_STEP = 512;
  std::string& scan = scanByFont_[fontId].textByStyle[static_cast<uint8_t>(style) & 0x03];
  if (scan.size() >= SCAN_BYTES_PER_STYLE) return;
  const size_t take = std::min(strlen(text), SCAN_BYTES_PER_STYLE - scan.size());
  const size_t needed = scan.size() + take;
  if (needed > scan.capacity()) {
    const size_t want =
        std::min(SCAN_BYTES_PER_STYLE, (needed + SCAN_GROWTH_STEP - 1) / SCAN_GROWTH_STEP * SCAN_GROWTH_STEP);
    // std::string growth cannot fail gracefully (no exceptions): a reserve the heap cannot meet
    // aborts the device. X3, 2026-10-01: a page draw at 14.7 KB free with a 4.3 KB largest block
    // ended in abort() here (operator new -> __terminate). The scan only feeds the prewarm, which is
    // an optimisation, so ask the allocator first; when it cannot spare the block, the rest of this
    // style goes unscanned and its glyphs load on demand as they are drawn. libstdc++ grows a
    // reserve to at least twice the old capacity, so that is the block asked about.
    if (!heapHasBlockFor(std::max(want, 2 * scan.capacity()) + 1)) return;
    scan.reserve(want);
  }
  if (style & EpdFontFamily::SMALL_CAPS) {
    appendSmallCapsFolded(scan, text, take);  // stays within the capacity reserved for `take`
  } else {
    scan.append(text, take);
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->resetStats();
  manager_->scanByFont_.clear();
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanByFont_.empty()) return;

  // One batch clear for the whole page; the per-(font,style) prewarmCache calls below
  // append into the freed slots (FontDecompressor dedupes across slots between calls).
  manager_->clearCache();

  // Prewarm every (font, style) pair that appeared during the scan, each with only the text
  // drawn in that style. The page slots are limited (MAX_PAGE_SLOTS), so warm the pair with
  // the MOST text first: the body font's dominant style (~hundreds of glyphs) must win the
  // slots over a short heading or a few styled words. Losers fall back cheaply per glyph —
  // far better than the body thrashing.
  struct PrewarmItem {
    int fontId;
    uint8_t style;
    const std::string* text;
  };
  std::vector<PrewarmItem> order;
  order.reserve(manager_->scanByFont_.size() * 4);  // worst case: every style seen in every font
  for (const auto& kv : manager_->scanByFont_) {
    for (uint8_t i = 0; i < 4; i++) {
      const std::string& text = kv.second.textByStyle[i];
      if (!text.empty()) order.push_back({kv.first, i, &text});
    }
  }
  std::sort(order.begin(), order.end(),
            [](const PrewarmItem& a, const PrewarmItem& b) { return a.text->size() > b.text->size(); });

  for (const auto& item : order) {
    manager_->prewarmCache(item.fontId, item.text->c_str(), static_cast<uint8_t>(1u << item.style));
  }

  manager_->scanByFont_.clear();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanByFont_ is empty)
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }

FontCacheManager::ScopedSlotArena::ScopedSlotArena(FontCacheManager& manager, BuildArena* arena) : manager_(manager) {
  if (!arena || !arena->valid() || !manager_.fontDecompressor_) return;
  // One reserved block for the whole page. Every slot bump-allocates inside it and the lot is
  // rewound in the destructor, so the arena cursor ends exactly where it started -- important
  // because the region is on loan from a live section build, which resumes using it afterwards.
  block_ = arena->reserveBlock();
  if (!block_.valid()) return;
  // Start from a clean slot table: a heap-backed slot left over from the previous page would
  // otherwise survive into this scope and be indistinguishable from an arena-backed one at
  // teardown. Prewarm rebuilds slot buffers on every call, so nothing useful is discarded.
  manager_.clearCache();
  manager_.fontDecompressor_->setSlotArena(arena);
  arena_ = arena;
  installed_ = true;
}

FontCacheManager::ScopedSlotArena::~ScopedSlotArena() {
  if (!installed_) return;
  // Clearing must precede uninstalling: freePageBuffer() decides whether to free() a slot by
  // reading the arena-backed flag, and with the arena already gone it would call free() on
  // interior pointers into a region this code does not own. That ordering is the reason this
  // class exists rather than two calls at each site.
  manager_.clearCache();
  manager_.fontDecompressor_->setSlotArena(nullptr);
  if (!arena_->release(block_)) {
    // Only reachable if something else allocated from the arena inside this scope and did not
    // release -- i.e. the render lock stopped excluding the build. Not fatal (the arena is reset
    // at the next build), but it means the invariant this class relies on has been broken.
    LOG_ERR("FCM", "Slot arena release refused: another owner allocated inside the page scope");
  }
}
