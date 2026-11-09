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
  master_ = nullptr;
  for (uint8_t& lock : faceLocks_) lock = 0;
  if (!sources_[Regular].present) return false;
  pointSize_ = pointSize;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72, kept in 26.6 so the fractional part survives
  // (glyphs raster and advance at the exact ppem; the per-face sizePx is the
  // rounded form for the integer-pixel Font API).
  size26_6_ = (static_cast<uint32_t>(pointSize) * FONT_RASTER_DPI * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  resolveFaces();
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
    f.ft = &f.ftStorage_;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
      f.slots[s].cap = glyphCacheBytes;
      f.slots[s].maxGlyphs = maxGlyphs;
    }
    f.inited = false;
    f.ready = false;
    f.initFailures = 0;
    // Flush the glyph/kern caches, not just the arena cursor: faultGlyph
    // consults the lookup tables before anything else, so entries rasterized
    // at a previous size would serve stale metrics — and their bitmaps get
    // overwritten as new-size glyphs refill the arena from offset 0.
    flushFace(f);
    f.ligPairCount = 0;                  // re-resolved in initFace; stale pairs must not leak
    for (uint32_t& g : f.ligGid) g = 0;  // across a reload with new sources
  }
  resetSlots();
  initFace(faces_[0]);  // regular eagerly: validates the font + gives metrics
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);  // handlers + placeholder metrics (regular's)
  loaded_ = true;
  return true;
}

bool TtfEpdFont::loadVariantOf(TtfEpdFont& master, const uint16_t pointSize, const bool twoBit,
                               const size_t glyphCacheBytes, const uint16_t maxGlyphs) {
  loaded_ = false;
  TtfEpdFont* root = &master;
  while (root->master_) root = root->master_;
  if (!root->loaded_) return false;
  master_ = root;
  for (uint8_t& lock : faceLocks_) lock = 0;
  pointSize_ = pointSize;
  size26_6_ = (static_cast<uint32_t>(pointSize) * FONT_RASTER_DPI * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    const Face& mf = root->faces_[i];
    f.owner = this;
    f.ft = mf.ft;
    f.srcIndex = mf.srcIndex;
    f.weight = mf.weight;
    f.wantItalic = mf.wantItalic;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
      f.slots[s].cap = glyphCacheBytes;
      f.slots[s].maxGlyphs = maxGlyphs;
    }
    f.inited = false;
    f.ready = false;
    f.initFailures = 0;
    flushFace(f);
    f.ligPairCount = 0;
    for (uint32_t& g : f.ligGid) g = 0;
  }
  resetSlots();
  initFace(faces_[0]);
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);
  loaded_ = true;
  return true;
}

void TtfEpdFont::initFace(Face& f) {
  if (master_) {
    const size_t idx = static_cast<size_t>(&f - faces_);
    Face& mf = master_->faces_[idx];
    if (f.inited && f.ready && mf.ready) return;
    if (f.initFailures >= 2) return;
    if (!mf.ready) master_->initFace(mf);
    f.ready = mf.ready;
    f.inited = mf.inited;
    if (!f.ready) {
      f.initFailures++;
      return;
    }
    f.initFailures = 0;
    f.ligPairCount = mf.ligPairCount;
    for (int i = 0; i < 5; ++i) {
      f.ligGid[i] = mf.ligGid[i];
      f.ligPairs[i] = mf.ligPairs[i];
    }
    setupFace(f);
    return;
  }
  if (f.inited && f.ready) return;
  if (f.initFailures >= 2) return;
  const Source& s = sources_[f.srcIndex];
  if (!s.present) {
    f.ready = false;
    f.inited = true;
    return;
  }
  const auto tryInit = [&]() {
    return s.streamed ? f.ft->initStream(s.read, s.ctx, s.fileSize, f.sizePx, f.weight, f.wantItalic)
                      : f.ft->init(s.data, s.len, f.sizePx, f.weight, f.wantItalic);
  };
  f.ready = tryInit();
  if (!f.ready) {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
    f.ready = tryInit();
  }
  if (!f.ready) {
    // Transient failure under heap pressure; leave f.inited false so a later fault can retry,
    // but bound retries so a persistent failure doesn't thrash ensureFree on every glyph.
    f.inited = false;
    f.initFailures++;
    return;
  }
  f.initFailures = 0;
  f.inited = true;
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
    if (!f.ft->setRenderOptions(ro)) {
      LOG_ERR("TTF", "Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not compiled)");
    }
    // GPOS kerning for RESIDENT faces only (they borrow a view into the font
    // bytes — free). Unlike GSUB, GPOS must stay resident for render-time
    // pair queries, and a STREAMED face would need an owned DRAM copy exactly
    // when the C3 is poorest (a font big enough to stream, i.e. CJK, where
    // Latin pair kerning barely matters). Budget 0 = streamed faces skip it.
    f.ft->setGposByteBudget(0);
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
  f.ft->setGsubByteBudget(48 * 1024);  // C3 DRAM discipline: oversized GSUB → no ligatures, not OOM
  for (int i = 0; i < 5; ++i) f.ligGid[i] = f.ft->ligatureGlyphId(kComps[i], kComps[i][2] ? 3 : 2);
  f.ft->releaseLigatureTable();

  // Pair table for EpdFont::applyLigatures(), keyed on the U+FB00–FB04
  // presentation codepoints. A ligature is usable if GSUB named a glyph or the
  // cmap maps the presentation codepoint directly. Entries are appended in
  // ascending key order (the ff/fi/fl keys sort below the FB00-chained ones).
  auto avail = [&f](const int i) { return f.ligGid[i] != 0 || f.ft->hasGlyph(0xFB00u + i); };
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
  freeink::font::FtFont& src = f.ready ? *f.ft : *faces_[0].ft;
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
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    f.slots[s].flush();
  }
  f.kernKeys.clear();
  f.kernVals.clear();
}

bool TtfEpdFont::activatePage(const int spineIndex, const int pageNumber) {
  if (spineIndex < 0 || pageNumber < 0) return false;
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    if (slotMeta_[s].spineIndex == spineIndex && slotMeta_[s].pageNumber == pageNumber) {
      activeSlot_ = s;
      targetSlot_ = s;
      slotMeta_[s].lastUse = ++slotClock_;
      return true;
    }
  }
  return false;
}

void TtfEpdFont::prepareTargetPage(const int spineIndex, const int pageNumber, const bool lookahead) {
  if (spineIndex < 0 || pageNumber < 0) return;
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    if (slotMeta_[s].spineIndex == spineIndex && slotMeta_[s].pageNumber == pageNumber) {
      targetSlot_ = s;
      slotMeta_[s].lastUse = ++slotClock_;
      if (!lookahead) activeSlot_ = s;
      return;
    }
  }
  uint8_t victim = 0;
  uint32_t minUse = UINT32_MAX;
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    if (lookahead && s == activeSlot_) continue;
    if (slotMeta_[s].spineIndex == -1) {
      victim = s;
      break;
    }
    if (slotMeta_[s].lastUse < minUse) {
      minUse = slotMeta_[s].lastUse;
      victim = s;
    }
  }
  for (Face& f : faces_) {
    f.slots[victim].flush();
  }
  slotMeta_[victim].spineIndex = spineIndex;
  slotMeta_[victim].pageNumber = pageNumber;
  slotMeta_[victim].lastUse = ++slotClock_;
  targetSlot_ = victim;
  if (!lookahead) activeSlot_ = victim;
}

void TtfEpdFont::restoreActiveSlot() { targetSlot_ = activeSlot_; }

void TtfEpdFont::clearTargetSlot() {
  for (Face& f : faces_) {
    if (f.inited) f.slots[targetSlot_].flush();
  }
}

bool TtfEpdFont::reclaimInactiveSlot() {
  int victim = -1;
  uint32_t minUse = UINT32_MAX;
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    if (s == activeSlot_ || s == targetSlot_ || slotMeta_[s].lastUse >= minUse) continue;
    for (const Face& f : faces_) {
      if (f.slots[s].bmp.capacity() > 0 || f.slots[s].glyphs.capacity() > 0) {
        minUse = slotMeta_[s].lastUse;
        victim = s;
        break;
      }
    }
  }
  if (victim < 0) return false;
  for (Face& f : faces_) f.slots[victim].release();
  slotMeta_[victim] = SlotMeta{};
  return true;
}

void TtfEpdFont::resetSlots() {
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    slotMeta_[s] = SlotMeta{};
  }
  activeSlot_ = 0;
  targetSlot_ = 0;
  slotClock_ = 0;
}

void TtfEpdFont::clearCache() {
  if (evictionLocked_) return;  // mid-fault: targetSlot and active faces are live
  // Drop cached glyphs across all slots on explicit cache invalidation.
  for (Face& f : faces_) {
    for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
      f.slots[s].flush();
    }
  }
  resetSlots();
}

void TtfEpdFont::releaseResidentCaches() {
  if (evictionLocked_) return;  // mid-fault: this font's faces are live
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.initFailures = 0;
    // Actually RELEASE the caches (swap-with-empty frees capacity; clear() alone
    // would not).
    for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
      f.slots[s].release();
    }
    freeink::font::PsramVector<uint64_t>().swap(f.kernKeys);
    freeink::font::PsramVector<int8_t>().swap(f.kernVals);
    // Keep the regular face's FreeType face live so coverage()/metrics still
    // answer without a reload (mirrors SD keeping its interval table resident).
    // Shed the lazy bold/italic/bold-italic faces entirely; they re-init on the
    // next glyph fault for that style.
    if (i == 0) continue;
    if (master_) {
      f.inited = false;
      f.ready = false;
      continue;
    }
    if (faceLocks_[i] > 0) continue;
    if (f.inited) {
      f.ft->deinit();
      f.inited = false;
      f.ready = false;
    }
  }
  resetSlots();
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp) {
  if (!f.inited || (master_ && !master_->faces_[&f - faces_].ready)) initFace(f);
  if (!f.ready) return nullptr;

  const uint8_t target = targetSlot_;
  GlyphSlot& targetSlot = f.slots[target];

  // 1. Check target slot first (direct cache hit).
  {
    const auto it = std::lower_bound(targetSlot.cps.begin(), targetSlot.cps.end(), cp);
    if (it != targetSlot.cps.end() && *it == cp) {
      const size_t gIdx = targetSlot.slot[static_cast<size_t>(it - targetSlot.cps.begin())];
      return &targetSlot.glyphs[gIdx];
    }
  }

  // Helper growth lambdas: exact and heap-checked.
  static constexpr size_t kTableStep = 64;
  const auto growTables = [&](GlyphSlot& s) {
    return reserveChecked(s.glyphs, s.glyphs.size() + 1, kTableStep, s.maxGlyphs) &&
           reserveChecked(s.cps, s.cps.size() + 1, kTableStep, s.maxGlyphs) &&
           reserveChecked(s.slot, s.slot.size() + 1, kTableStep, s.maxGlyphs);
  };
  const auto evictOthers = [&]() {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
  };
  const auto insertGlyph = [&](const EpdGlyph& eg) -> const EpdGlyph* {
    targetSlot.glyphs.push_back(eg);
    const uint16_t newIdx = static_cast<uint16_t>(targetSlot.glyphs.size() - 1);
    const auto it = std::lower_bound(targetSlot.cps.begin(), targetSlot.cps.end(), cp);
    const size_t pos = static_cast<size_t>(it - targetSlot.cps.begin());
    targetSlot.cps.insert(it, cp);
    targetSlot.slot.insert(targetSlot.slot.begin() + pos, newIdx);
    return &targetSlot.glyphs[newIdx];
  };

  // 2. Check other slots. If found, copy the rasterized glyph into targetSlot
  // without touching FreeType or reading from SD (instant copy path).
  for (uint8_t s = 0; s < NUM_GLYPH_SLOTS; ++s) {
    if (s == target) continue;
    GlyphSlot& srcSlot = f.slots[s];
    const auto it = std::lower_bound(srcSlot.cps.begin(), srcSlot.cps.end(), cp);
    if (it != srcSlot.cps.end() && *it == cp) {
      const EpdGlyph srcG = srcSlot.glyphs[srcSlot.slot[static_cast<size_t>(it - srcSlot.cps.begin())]];
      const size_t bytes = srcG.dataLength;
      if (bytes > targetSlot.cap) continue;
      if (targetSlot.glyphs.size() >= targetSlot.maxGlyphs || targetSlot.used + bytes > targetSlot.cap) {
        targetSlot.flush();
      }
      if (!growTables(targetSlot)) {
        evictOthers();
        if (!growTables(targetSlot)) return nullptr;
      }
      if (bytes > 0) {
        if (!reserveChecked(targetSlot.bmp, targetSlot.used + bytes, 4096, targetSlot.cap)) {
          evictOthers();
          if (!reserveChecked(targetSlot.bmp, targetSlot.used + bytes, 4096, targetSlot.cap)) {
            // Cannot allocate bitmap in targetSlot: do not poison cache with a blank entry
            continue;
          }
        }
      }
      EpdGlyph eg = srcG;
      if (bytes > 0) {
        if (targetSlot.bmp.size() < targetSlot.used + bytes) targetSlot.bmp.resize(targetSlot.used + bytes, 0);
        memcpy(targetSlot.bmp.data() + targetSlot.used, srcSlot.bmp.data() + srcG.dataOffset, bytes);
        eg.dataOffset = static_cast<uint32_t>(targetSlot.used);
        targetSlot.used += bytes;
      } else {
        eg.dataOffset = static_cast<uint32_t>(targetSlot.used);
      }
      return insertGlyph(eg);
    }
  }

  // 3. Not in any slot: rasterize via FreeType into targetSlot.
  freeink::font::FtFont::GlyphId gid = f.ft->glyphId(cp);
  if (gid == 0 && cp >= 0xFB00u && cp <= 0xFB04u) gid = f.ligGid[cp - 0xFB00u];
  if (gid == 0 && cp != REPLACEMENT_GLYPH) return nullptr;

  const size_t faceIdx = static_cast<size_t>(&f - faces_);
  FaceLock lock(master_ ? master_ : this, faceIdx);

  // Glyph-ID render at the exact fractional ppem. The metrics pass costs a
  // second glyph load per MISS only (hits come from the cache) and yields the
  // 26.6 advance, which the 12.4 EpdGlyph.advanceX preserves for the
  // renderer's differential rounding — GlyphBitmap.advance is whole pixels.
  const uint32_t size26_6 = f.owner->size26_6_;
  freeink::font::FtFont::GlyphMetrics gm;
  const bool haveMetrics = f.ft->metricsGlyph26_6(gid, size26_6, gm);
  const freeink::font::GlyphBitmap* g = f.ft->rasterizeGlyph26_6(gid, size26_6);
  if (!g) {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
    reclaimInactiveSlot();
    g = f.ft->rasterizeGlyph26_6(gid, size26_6);
  }
  if (!g) {
    // Rasterization failed: do not pollute cache with a blank or dummy entry.
    // Returning nullptr causes EpdFont::getGlyph() to fall back to REPLACEMENT_GLYPH (tofu),
    // and leaves this codepoint uncached so subsequent passes retry rasterization.
    return nullptr;
  }
  uint32_t px = g->pixels ? static_cast<uint32_t>(g->width) * g->height : 0;
  size_t bytes = px ? (f.twoBit ? (px + 3) / 4 : (px + 7) / 8) : 0;
  if (bytes > targetSlot.cap) {
    // Glyph bitmap exceeds slot capacity: do not cache a broken zero-byte entry.
    // Returning nullptr triggers fallback to REPLACEMENT_GLYPH (tofu) and keeps cp uncached.
    return nullptr;
  }
  if (targetSlot.glyphs.size() >= targetSlot.maxGlyphs || targetSlot.used + bytes > targetSlot.cap) {
    targetSlot.flush();
  }

  if (!growTables(targetSlot)) {
    evictOthers();
    if (!growTables(targetSlot)) {
      if (!reclaimInactiveSlot() || !growTables(targetSlot)) return nullptr;
    }
  }
  if (px && !reserveChecked(targetSlot.bmp, targetSlot.used + bytes, 4096, targetSlot.cap)) {
    evictOthers();
    if (!reserveChecked(targetSlot.bmp, targetSlot.used + bytes, 4096, targetSlot.cap)) {
      if (!reclaimInactiveSlot() || !reserveChecked(targetSlot.bmp, targetSlot.used + bytes, 4096, targetSlot.cap)) {
        // Bitmap allocation failed in targetSlot: do not cache a broken zero-byte entry.
        // Returning nullptr triggers fallback to REPLACEMENT_GLYPH (tofu) and keeps cp uncached.
        return nullptr;
      }
    }
  }

  EpdGlyph eg{};
  const int32_t adv12_4 = haveMetrics ? (gm.advance26_6 + 2) >> 2 : (g->advance << 4);
  eg.advanceX = static_cast<uint16_t>(adv12_4 < 0 ? 0 : adv12_4);
  if (px && g->pixels) {
    // Grow the byte arena on demand toward the book's page high-water mark.
    // flush above guarantees targetSlot.used + bytes <= targetSlot.cap, so this
    // never exceeds the ceiling; capacity is retained across page turns.
    if (targetSlot.bmp.size() < targetSlot.used + bytes) {
      targetSlot.bmp.resize(targetSlot.used + bytes, 0);
    }
    uint8_t* dst = targetSlot.bmp.data() + targetSlot.used;
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
    eg.dataOffset = static_cast<uint32_t>(targetSlot.used);
    eg.dataLength = static_cast<uint16_t>(bytes);
    targetSlot.used += bytes;
  } else {
    // Valid empty glyph (e.g. space)
    eg.width = 0;
    eg.height = 0;
    eg.dataOffset = static_cast<uint32_t>(targetSlot.used);
    eg.dataLength = 0;
  }

  return insertGlyph(eg);
}

int8_t TtfEpdFont::faultKern(Face& f, const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp == 0 || rightCp == 0) return 0;
  if (!f.inited || (master_ && !master_->faces_[&f - faces_].ready)) initFace(f);
  if (!f.ready) return 0;
  const uint64_t key = (uint64_t(leftCp) << 32) | rightCp;
  {
    const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
    if (it != f.kernKeys.end() && *it == key) return f.kernVals[static_cast<size_t>(it - f.kernKeys.begin())];
  }

  // 26.6 → 4.4 (round-half-away), clamped to the int8 contract (±8px, far
  // beyond any real kern at reader sizes).
  const int32_t k26 = f.ft->kerning26_6(leftCp, rightCp, f.owner->size26_6_);
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
  Face* f = static_cast<Face*>(ctx);
  const uint8_t active = f->owner ? f->owner->activeSlot() : 0;
  for (uint8_t i = 0; i < NUM_GLYPH_SLOTS; ++i) {
    const GlyphSlot& s = f->slots[(active + i) % NUM_GLYPH_SLOTS];
    if (glyph >= s.glyphs.data() && glyph < s.glyphs.data() + s.glyphs.size()) {
      return s.bmp.data() + glyph->dataOffset;
    }
  }
  return nullptr;
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  // All styles share one file → coverage comes from the always-live regular face.
  Face& reg = static_cast<Face*>(ctx)->owner->faces_[0];
  if (reg.ft->hasGlyph(codepoint)) return true;
  // Ligature presentation codepoints resolvable through GSUB despite no cmap
  // entry (some EPUBs carry literal U+FB01/U+FB02 in their text).
  return codepoint >= 0xFB00u && codepoint <= 0xFB04u && reg.ligGid[codepoint - 0xFB00u] != 0;
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(pointSize_, &faces_[0].font, &faces_[1].font, &faces_[2].font, &faces_[3].font);
}

bool TtfEpdFont::build(const char* utf8, const uint8_t style) {
  if (!loaded_ || style >= 4) return false;
  faces_[style].slots[targetSlot_].flush();
  return addCoverage(utf8, style);
}
bool TtfEpdFont::addCoverage(const char* utf8, const uint8_t style) {
  if (!loaded_ || utf8 == nullptr || style >= 4) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != '\0') faultGlyph(faces_[style], utf8NextCodepoint(&p));
  return true;
}

#endif  // CROSSPOINT_VECTOR_FONTS
