#include "TtfEpdFont.h"

#if CROSSPOINT_VECTOR_FONTS

#include <Logging.h>
#include <MemoryManager.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>

namespace {

// Cache growth policy: PsramAlloc ABORTS when fiFontMalloc fails, and vector
// doubling transiently needs old+new blocks, so every cache expansion must be
// an exact, heap-checked reserve. On no-PSRAM boards the block lands in
// internal DRAM and must leave working headroom for the rest of the system.
bool canGrow(const size_t bytes) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bytes) return true;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + 12 * 1024;
}

// Exact reserve to `need` rounded up to `step`, capped at `ceil`. Returns
// false (leaving the vector untouched) when the heap cannot fund it.
template <typename V>
bool reserveChecked(V& v, const size_t need, const size_t step, const size_t ceil) {
  if (v.capacity() >= need) return true;
  size_t newCap = ((need + step - 1) / step) * step;
  if (newCap > ceil) newCap = ceil;
  if (newCap < need) newCap = need;  // need may legitimately exceed ceil rounding
  if (!canGrow(newCap * sizeof(typename V::value_type))) return false;
  v.reserve(newCap);
  return true;
}
}  // namespace

void TtfEpdFont::addResidentSource(const uint8_t style, const uint8_t* data, const uint32_t len) {
  if (style >= 4 || data == nullptr || len == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = false;
  s.data = data;
  s.len = len;
}

void TtfEpdFont::addStreamSource(const uint8_t style, const freeink::font::FtFont::ReadFn read, void* ctx,
                                 const unsigned long fileSize) {
  if (style >= 4 || read == nullptr || fileSize == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = true;
  s.read = read;
  s.ctx = ctx;
  s.fileSize = fileSize;
}

void TtfEpdFont::resolveFaces() {
  // Map each style face onto the best available source. Regular (0) anchors the
  // family; the others prefer an explicit file, then fall back to synthesizing
  // from a related source (wght axis / faux bold for weight, ital axis / oblique
  // for slant — all handled inside FtFont).
  const bool haveB = sources_[Bold].present;
  const bool haveI = sources_[Italic].present;
  const bool haveBI = sources_[BoldItalic].present;

  // regular
  faces_[Regular].srcIndex = Regular;
  faces_[Regular].weight = 400;
  faces_[Regular].wantItalic = false;

  // bold
  faces_[Bold].srcIndex = haveB ? Bold : Regular;
  faces_[Bold].weight = haveB ? 400 : 700;
  faces_[Bold].wantItalic = false;

  // italic
  faces_[Italic].srcIndex = haveI ? Italic : Regular;
  faces_[Italic].weight = 400;
  faces_[Italic].wantItalic = !haveI;  // oblique/axis only when using the roman source

  // bold-italic: dedicated file > bold-of-italic-file > italic-of-bold-file > roman
  Face& bi = faces_[BoldItalic];
  if (haveBI) {
    bi.srcIndex = BoldItalic;
    bi.weight = 400;
    bi.wantItalic = false;
  } else if (haveI) {
    bi.srcIndex = Italic;
    bi.weight = 700;  // wght axis / faux bold on the italic design
    bi.wantItalic = false;
  } else if (haveB) {
    bi.srcIndex = Bold;
    bi.weight = 400;
    bi.wantItalic = true;  // oblique on the bold design
  } else {
    bi.srcIndex = Regular;
    bi.weight = 700;
    bi.wantItalic = true;
  }
}

bool TtfEpdFont::load(const uint16_t pointSize, const bool twoBit, const size_t glyphCacheBytes,
                      const uint16_t maxGlyphs) {
  loaded_ = false;
  if (!sources_[Regular].present) return false;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72, kept in 26.6 so the fractional part survives
  // (glyphs raster and advance at the exact ppem; the per-face sizePx is the
  // rounded form for the integer-pixel Font API).
  size26_6_ = (static_cast<uint32_t>(pointSize) * 150u * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  resolveFaces();
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    f.cap = glyphCacheBytes;
    f.maxGlyphs = maxGlyphs;
    f.inited = false;
    f.ready = false;
    // Flush the glyph/kern caches, not just the arena cursor: faultGlyph
    // consults the lookup tables before anything else, so entries rasterized
    // at a previous size would serve stale metrics — and their bitmaps get
    // overwritten as new-size glyphs refill the arena from offset 0.
    flushFace(f);
    f.ligPairCount = 0;                  // re-resolved in initFace; stale pairs must not leak
    for (uint32_t& g : f.ligGid) g = 0;  // across a reload with new sources
  }
  initFace(faces_[0]);  // regular eagerly: validates the font + gives metrics
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);  // handlers + placeholder metrics (regular's)
  loaded_ = true;
  return true;
}

void TtfEpdFont::initFace(Face& f) {
  if (f.inited) return;
  f.inited = true;
  const Source& s = sources_[f.srcIndex];
  if (!s.present) {
    f.ready = false;
  } else if (s.streamed) {
    f.ready = f.ft.initStream(s.read, s.ctx, s.fileSize, f.sizePx, f.weight, f.wantItalic);
  } else {
    f.ready = f.ft.init(s.data, s.len, f.sizePx, f.weight, f.wantItalic);
  }
  if (f.ready) {
    // Full auto-hinting + stem darkening pair with the BW-aware quantizer in
    // faultGlyph: the 50% ink threshold needs stems snapped to full-coverage
    // pixels or strokes break up / vary per letter on BW page turns (see
    // platformio.ini's FREEINK_FONT_ENABLE_AUTOHINT note). Auto (both axes)
    // rather than Light (vertical only): Light leaves stem WIDTHS fractional,
    // so the ink threshold prints the same-width stem as 1px or 2px depending
    // on each glyph's horizontal phase — visibly uneven letter weights. Auto
    // equalizes stem widths across the face. Darkening nudges borderline-thin
    // strokes over the threshold.
    freeink::font::FtFont::RenderOptions ro;
    ro.hinting = freeink::font::FtFont::HintingMode::Auto;
    ro.stemDarkening = true;
    if (!f.ft.setRenderOptions(ro)) {
      LOG_ERR("TTF", "Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not compiled)");
    }
    // GPOS kerning for RESIDENT faces only (they borrow a view into the font
    // bytes — free). Unlike GSUB, GPOS must stay resident for render-time
    // pair queries, and a STREAMED face would need an owned DRAM copy exactly
    // when the C3 is poorest (a font big enough to stream, i.e. CJK, where
    // Latin pair kerning barely matters). Budget 0 = streamed faces skip it.
    f.ft.setGposByteBudget(0);
    resolveLigatures(f);
  }
  // Caches are NOT pre-reserved: the byte arena (f.bmp) and the glyph tables
  // grow on demand in faultGlyph and converge on the book's page needs (see the
  // header's memory note). f.cap is only the hard ceiling that triggers a flush.
  setupFace(f);
}

void TtfEpdFont::resolveLigatures(Face& f) {
  // One GSUB pass per face init: resolve the standard Latin ligatures to glyph
  // IDs, then drop the table — nothing else queries GSUB, so a streamed face
  // never keeps its (budgeted) table copy resident.
  static constexpr uint32_t kComps[5][3] = {
      {'f', 'f', 0}, {'f', 'i', 0}, {'f', 'l', 0}, {'f', 'f', 'i'}, {'f', 'f', 'l'}};
  f.ft.setGsubByteBudget(48 * 1024);  // C3 DRAM discipline: oversized GSUB → no ligatures, not OOM
  for (int i = 0; i < 5; ++i) f.ligGid[i] = f.ft.ligatureGlyphId(kComps[i], kComps[i][2] ? 3 : 2);
  f.ft.releaseLigatureTable();

  // Pair table for EpdFont::applyLigatures(), keyed on the U+FB00–FB04
  // presentation codepoints. A ligature is usable if GSUB named a glyph or the
  // cmap maps the presentation codepoint directly. Entries are appended in
  // ascending key order (the ff/fi/fl keys sort below the FB00-chained ones).
  auto avail = [&f](const int i) { return f.ligGid[i] != 0 || f.ft.hasGlyph(0xFB00u + i); };
  f.ligPairCount = 0;
  auto add = [&f](const uint32_t left, const uint32_t right, const uint32_t out) {
    f.ligPairs[f.ligPairCount++] = EpdLigaturePair{(left << 16) | right, out};
  };
  if (avail(0)) add('f', 'f', 0xFB00);
  if (avail(1)) add('f', 'i', 0xFB01);
  if (avail(2)) add('f', 'l', 0xFB02);
  if (avail(0)) {  // ffi/ffl chain through the ff result as the new left
    if (avail(3)) add(0xFB00, 'i', 0xFB03);
    if (avail(4)) add(0xFB00, 'l', 0xFB04);
  }
}

void TtfEpdFont::setupFace(Face& f) {
  // Metrics from this face once live, else borrow the (always-live) regular
  // face's — same font/size, so a fine placeholder until this face is faulted.
  freeink::font::FtFont& src = f.ready ? f.ft : faces_[0].ft;
  // Fractional-ppem line metrics, rounded once here rather than per-call.
  freeink::font::FtFont::LineMetrics lm;
  int ascent, lineHeight;
  if (src.lineMetrics26_6(f.owner->size26_6_, lm)) {
    ascent = static_cast<int>((lm.ascender26_6 + 32) >> 6);
    lineHeight = static_cast<int>((lm.height26_6 + 32) >> 6);
  } else {
    ascent = src.ascent(f.sizePx);
    lineHeight = src.lineHeight(f.sizePx);
  }
  f.data = EpdFontData{};
  f.data.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  f.data.ascender = ascent;
  f.data.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  f.data.is2Bit = f.twoBit;
  f.data.glyphMissHandler = &TtfEpdFont::missThunk;
  f.data.glyphMissCtx = &f;
  f.data.coverageHandler = &TtfEpdFont::coverageThunk;
  f.data.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;
  // GSUB-derived ligature pairs (resolveLigatures). Empty until this face
  // inits — a lazy style renders its first pass ligature-free, then picks
  // them up once its glyphs fault the face in.
  f.data.ligaturePairs = f.ligPairCount ? f.ligPairs : nullptr;
  f.data.ligaturePairCount = f.ligPairCount;
  f.data.kernHandler = &TtfEpdFont::kernThunk;
}

void TtfEpdFont::flushFace(Face& f) {
  f.glyphs.clear();
  f.cps.clear();
  f.slot.clear();
  f.kernKeys.clear();
  f.kernVals.clear();
  f.used = 0;
}

void TtfEpdFont::clearCache() {
  // Drop cached page glyphs on every inited face but keep the allocations: the
  // vectors keep capacity (clear() does not free) and f.bmp keeps its buffer, so
  // the next prewarm re-faults into buffers already sized to the book.
  for (Face& f : faces_) {
    if (f.inited) flushFace(f);
  }
}

void TtfEpdFont::releaseResidentCaches() {
  if (evictionLocked_) return;  // mid-fault: this font's faces are live
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    // Actually RELEASE the caches (swap-with-empty frees capacity; clear() alone
    // would not).
    flushFace(f);
    freeink::font::PsramVector<uint8_t>().swap(f.bmp);
    freeink::font::PsramVector<EpdGlyph>().swap(f.glyphs);
    freeink::font::PsramVector<uint32_t>().swap(f.cps);
    freeink::font::PsramVector<uint16_t>().swap(f.slot);
    freeink::font::PsramVector<uint64_t>().swap(f.kernKeys);
    freeink::font::PsramVector<int8_t>().swap(f.kernVals);
    // Keep the regular face's FreeType face live so coverage()/metrics still
    // answer without a reload (mirrors SD keeping its interval table resident).
    // Shed the lazy bold/italic/bold-italic faces entirely; they re-init on the
    // next glyph fault for that style.
    if (i == 0) continue;
    if (f.inited) {
      f.ft.deinit();
      f.inited = false;
      f.ready = false;
    }
  }
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp) {
  if (!f.inited) initFace(f);
  if (!f.ready) return nullptr;
  {
    const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
    if (it != f.cps.end() && *it == cp) return &f.glyphs[f.slot[static_cast<size_t>(it - f.cps.begin())]];
  }

  // Resolve to a glyph ID: cmap first, then the GSUB result for the ligature
  // presentation codepoints (whose glyphs commonly have no cmap entry at all).
  freeink::font::FtFont::GlyphId gid = f.ft.glyphId(cp);
  if (gid == 0 && cp >= 0xFB00u && cp <= 0xFB04u) gid = f.ligGid[cp - 0xFB00u];
  if (gid == 0) return nullptr;

  // Glyph-ID render at the exact fractional ppem. The metrics pass costs a
  // second glyph load per MISS only (hits come from the cache) and yields the
  // 26.6 advance, which the 12.4 EpdGlyph.advanceX preserves for the
  // renderer's differential rounding — GlyphBitmap.advance is whole pixels.
  const uint32_t size26_6 = f.owner->size26_6_;
  freeink::font::FtFont::GlyphMetrics gm;
  const bool haveMetrics = f.ft.metricsGlyph26_6(gid, size26_6, gm);
  const freeink::font::GlyphBitmap* g = f.ft.rasterizeGlyph26_6(gid, size26_6);
  uint32_t px = (g && g->pixels) ? static_cast<uint32_t>(g->width) * g->height : 0;
  size_t bytes = px ? (f.twoBit ? (px + 3) / 4 : (px + 7) / 8) : 0;
  if (bytes > f.cap) {
    bytes = 0;
    px = 0;
  }
  if (f.glyphs.size() >= f.maxGlyphs || f.used + bytes > f.cap) flushFace(f);

  // All growth below is exact and heap-checked (see reserveChecked). On a
  // failed grow, ask the memory manager to evict rebuildable caches (render
  // glyph cache, other fonts' arenas — evictionLocked_ keeps THIS font's
  // faces alive) and retry once. After that: a failed table grow is an
  // uncached miss (caller skips the glyph this pass); a failed arena grow
  // degrades to an advance-only glyph so layout survives.
  static constexpr size_t kTableStep = 64;
  const auto growTables = [&]() {
    return reserveChecked(f.glyphs, f.glyphs.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.cps, f.cps.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.slot, f.slot.size() + 1, kTableStep, f.maxGlyphs);
  };
  const auto evictOthers = [&]() {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
  };
  if (!growTables()) {
    evictOthers();
    if (!growTables()) return nullptr;
  }
  if (px && !reserveChecked(f.bmp, f.used + bytes, 4096, f.cap)) {
    evictOthers();
    if (!reserveChecked(f.bmp, f.used + bytes, 4096, f.cap)) {
      bytes = 0;
      px = 0;
    }
  }

  EpdGlyph eg{};
  const int32_t adv12_4 = haveMetrics ? (gm.advance26_6 + 2) >> 2 : (g ? g->advance << 4 : 0);
  eg.advanceX = static_cast<uint16_t>(adv12_4 < 0 ? 0 : adv12_4);
  if (px && g && g->pixels) {
    // Grow the byte arena on demand toward the book's page high-water mark.
    // flush above guarantees f.used + bytes <= f.cap, so this never exceeds the
    // ceiling; capacity is retained across page turns (clearCache keeps it).
    if (f.bmp.size() < f.used + bytes) f.bmp.resize(f.used + bytes, 0);
    uint8_t* dst = f.bmp.data() + f.used;
    for (size_t i = 0; i < bytes; ++i) dst[i] = 0;
    for (uint32_t i = 0; i < px; ++i) {
      const uint8_t a = g->pixels[i];
      if (f.twoBit) {
        // Same 25/50/75% level thresholds as the .cpfont converter
        // (fontconvert_sdcard.py's 4-bit >=4/8/12 downsample), so a TTF face
        // prints at the same stroke weight as its .cpfont rendition. The BW
        // page-turn pass inks ANY nonzero level, so the 25% start is what
        // gives CJK strokes their full weight; starting at 50% rendered
        // visibly thinner than cpfont.
        const uint8_t v = a < 64 ? 0 : a < 128 ? 1 : a < 192 ? 2 : 3;
        dst[i >> 2] |= static_cast<uint8_t>(v << ((3 - (i & 3)) * 2));
      } else if (a >= 128) {
        dst[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
    eg.width = static_cast<uint8_t>(g->width);
    eg.height = static_cast<uint8_t>(g->height);
    eg.left = g->xoff;
    // FreeInkFont's yoff is negative-above (top offset from baseline); CrossPoint's
    // EpdGlyph.top is positive-above (renderer computes screen Y = cursorY - top,
    // matching the .cpfont converter's top = FreeType bitmap_top). Flip the sign.
    eg.top = static_cast<int16_t>(-g->yoff);
    eg.dataOffset = static_cast<uint32_t>(f.used);
    eg.dataLength = static_cast<uint16_t>(bytes);
    f.used += bytes;
  } else {
    eg.dataOffset = static_cast<uint32_t>(f.used);
    eg.dataLength = 0;
  }

  f.glyphs.push_back(eg);
  const uint16_t newIdx = static_cast<uint16_t>(f.glyphs.size() - 1);
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.cps.begin());
  f.cps.insert(it, cp);
  f.slot.insert(f.slot.begin() + pos, newIdx);
  return &f.glyphs[newIdx];
}

int8_t TtfEpdFont::faultKern(Face& f, const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp == 0 || rightCp == 0) return 0;
  if (!f.inited) initFace(f);
  if (!f.ready) return 0;
  const uint64_t key = (uint64_t(leftCp) << 32) | rightCp;
  {
    const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
    if (it != f.kernKeys.end() && *it == key) return f.kernVals[static_cast<size_t>(it - f.kernKeys.begin())];
  }

  // 26.6 → 4.4 (round-half-away), clamped to the int8 contract (±8px, far
  // beyond any real kern at reader sizes).
  const int32_t k26 = f.ft.kerning26_6(leftCp, rightCp, f.owner->size26_6_);
  int32_t k4 = (k26 + (k26 < 0 ? -2 : 2)) / 4;
  if (k4 < -128) k4 = -128;
  if (k4 > 127) k4 = 127;

  // Text uses few distinct pairs; the cap only guards against pathological
  // content churning the cache without bound.
  static constexpr size_t kKernCap = 512;
  if (f.kernKeys.size() >= kKernCap) {
    f.kernKeys.clear();
    f.kernVals.clear();
  }
  // One-time exact reserve: with capacity pinned at the cap, the sorted
  // inserts below never touch the allocator. Uncached result if the heap
  // cannot fund the cache at all.
  if (!reserveChecked(f.kernKeys, kKernCap, kKernCap, kKernCap) ||
      !reserveChecked(f.kernVals, kKernCap, kKernCap, kKernCap)) {
    return static_cast<int8_t>(k4);
  }
  const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
  const size_t pos = static_cast<size_t>(it - f.kernKeys.begin());
  f.kernKeys.insert(it, key);
  f.kernVals.insert(f.kernVals.begin() + pos, static_cast<int8_t>(k4));
  return static_cast<int8_t>(k4);
}

const EpdGlyph* TtfEpdFont::missThunk(void* ctx, const uint32_t codepoint) {
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultGlyph(*f, codepoint);
}
int8_t TtfEpdFont::kernThunk(void* ctx, const uint32_t leftCp, const uint32_t rightCp) {
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultKern(*f, leftCp, rightCp);
}
const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr || glyph->dataLength == 0) return nullptr;
  return static_cast<Face*>(ctx)->bmp.data() + glyph->dataOffset;
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  // All styles share one file → coverage comes from the always-live regular face.
  Face& reg = static_cast<Face*>(ctx)->owner->faces_[0];
  if (reg.ft.hasGlyph(codepoint)) return true;
  // Ligature presentation codepoints resolvable through GSUB despite no cmap
  // entry (some EPUBs carry literal U+FB01/U+FB02 in their text).
  return codepoint >= 0xFB00u && codepoint <= 0xFB04u && reg.ligGid[codepoint - 0xFB00u] != 0;
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(&faces_[0].font, &faces_[1].font, &faces_[2].font, &faces_[3].font);
}

bool TtfEpdFont::build(const char* utf8) {
  if (!loaded_) return false;
  flushFace(faces_[0]);
  return addCoverage(utf8);
}
bool TtfEpdFont::addCoverage(const char* utf8) {
  if (!loaded_ || utf8 == nullptr) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != '\0') faultGlyph(faces_[0], utf8NextCodepoint(&p));
  return true;
}

#endif  // CROSSPOINT_VECTOR_FONTS
