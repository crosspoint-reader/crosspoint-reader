#include "TtfFont.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdint>

using glyphstore::kIdentityBytes;
using Commit = FlashGlyphStore::Commit;
using Lookup = FlashGlyphStore::Lookup;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

TtfFont::Hooks hooks;

// What a default-ignorable codepoint draws as: no advance and no ink, as the converter makes them (spec 8.1).
const EpdGlyph kIgnorable{};

constexpr uint32_t kCopyBytes = 4096;  // install: one SD read and one flash write per chunk
constexpr uint8_t kMaxFailuresLogged = 16;

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

}  // namespace

void TtfFont::setHooks(const Hooks& newHooks) { hooks = newHooks; }

TtfFont::TtfFont(NorFlash& flash) : flash_(flash), store_(flash) {}

void TtfFont::checkLock() const {
  static bool logged = false;
  if (hooks.renderLockHeld && !logged && !hooks.renderLockHeld()) {
    logged = true;
    LOG_ERR("TTF", "glyph rendered without RenderLock (spec 3.4)");
  }
}

bool TtfFont::load(const ttf::FileSource (&files)[4], const uint8_t identity[kIdentityBytes],
                   const std::function<void()>& onBusy) {
  if (files[0].size == 0) {
    LOG_ERR("TTF", "family has no regular style");
    return false;
  }
  bool installed = false;
  OpenResult opened = store_.open(identity);
  if (opened == OpenResult::NotInstalled || opened == OpenResult::OtherFamily || opened == OpenResult::MapFailed) {
    if (!install(files, identity, onBusy)) return false;
    installed = true;
    opened = store_.open(identity);
  }
  if (opened != OpenResult::Ready) {
    LOG_ERR("TTF", "glyph store did not open (%u)", static_cast<unsigned>(opened));
    return false;
  }

  ttf::ScratchPlan plan;
  StylesResult styles = openStyles(files, &plan);
  if (styles == StylesResult::MapFailed && !installed) {
    // The regions opened but a style no longer maps: the mapping budget shrank since the install (a firmware
    // update). Install again with a plan for today's budget (spec 6.2); a fresh install that still cannot map a
    // style is refused below rather than installed again at every load.
    LOG_INF("TTF", "a style no longer fits the mapping budget; installing the family again");
    if (!install(files, identity, onBusy)) return false;
    opened = store_.open(identity);
    if (opened != OpenResult::Ready) {
      LOG_ERR("TTF", "glyph store did not open after a reinstall (%u)", static_cast<unsigned>(opened));
      return false;
    }
    plan = ttf::ScratchPlan{};
    styles = openStyles(files, &plan);
  }
  if (styles != StylesResult::Ok) return false;

  // The output region doubles as the batch buffer, so it also holds the record header of its largest bitmap.
  plan.outputBytes = std::min(plan.outputBytes + glyphstore::kRecordHeaderBytes + 3, ttf::kMaxOutputBytes);
  if (!rasterizer_.allocate(plan)) return false;
  if (store_.wipePending()) wipeIfPending(onBusy);
  LOG_INF("TTF", "family loaded: %u B scratch, %u B per native size, log %lu of %lu B used",
          static_cast<unsigned>(rasterizer_.scratchBytes()), static_cast<unsigned>(sizeof(Size)),
          static_cast<unsigned long>(store_.logUsed()), static_cast<unsigned long>(store_.geometry().logSize));
  return true;
}

TtfFont::StylesResult TtfFont::openStyles(const ttf::FileSource (&files)[4], ttf::ScratchPlan* plan) {
  // Present styles take the store's style blocks in order, at install as here.
  uint8_t block = 0;
  maxNativePx_ = kMaxNativePx;
  for (uint8_t s = 0; s < 4; ++s) {
    Style& style = styles_[s];
    style.present = false;
    style.face = {};
    if (files[s].size == 0) continue;
    style.block = block++;
    glyphstore::StyleInfo info{};
    if (!store_.styleInfo(style.block, info)) return StylesResult::Failed;
    style.bytes = info.size;
    FontBytes bytes;
    if (!fontBytes(s, &bytes)) return StylesResult::MapFailed;
    if (ttf::parseFace(bytes, &style.face) != ttf::FaceStatus::Ok) {
      LOG_ERR("TTF", "style %u is not a font the engine draws", s);
      continue;
    }
    style.present = true;
    maxNativePx_ = std::min(maxNativePx_, nativeCeilingPx(style.face));
    // Planned for the largest size drawn: a native size (5 x 26 pt's pixels per em, about) above the body sizes.
    const uint32_t largest = std::max(ttf::ppemFromPoints(kMaxPointSize), static_cast<uint32_t>(kMaxNativePx) << 6);
    *plan = ttf::mergePlans(*plan, ttf::planScratch(style.face, largest, ttf::kPoolBytesFlash));
  }
  if (!styles_[0].present) return StylesResult::Failed;
  return StylesResult::Ok;
}

bool TtfFont::install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy) {
  store_.close();  // the plan needs the free mapping pages with nothing of the store mapped
  glyphstore::PlanInput in{};
  in.mode = glyphstore::kModeFlash;
  for (uint8_t s = 0; s < 4; ++s) {
    if (files[s].size != 0) in.styleBytes[in.styleCount++] = files[s].size;
  }
  in.usableBytes = flash_.size();
  in.freeMapPages = flash_.freeMapPages();
  glyphstore::Layout layout{};
  if (!glyphstore::planLayout(in, layout)) {
    LOG_ERR("TTF", "family does not fit the font partition (%u styles, %lu free mapping pages)", in.styleCount,
            static_cast<unsigned long>(in.freeMapPages));
    return false;
  }
  if (onBusy) onBusy();
  // Freed when the install ends; a 4 KB chunk keeps the copy at one SD read and one flash write per call.
  const auto buffer = makeUniqueNoThrow<uint8_t[]>(kCopyBytes);
  if (!buffer) {
    LOG_ERR("TTF", "install: no %lu B copy buffer", static_cast<unsigned long>(kCopyBytes));
    return false;
  }
  if (!store_.beginInstall(layout, identity, nullptr)) return false;
  uint8_t block = 0;
  for (uint8_t s = 0; s < 4; ++s) {
    const ttf::FileSource& file = files[s];
    if (file.size == 0) continue;
    for (uint32_t offset = 0; offset < file.size; offset += kCopyBytes) {
      const uint32_t len = std::min(kCopyBytes, file.size - offset);
      if (!file.read(file.ctx, offset, buffer.get(), len) || !store_.writeStyleData(block, offset, buffer.get(), len)) {
        LOG_ERR("TTF", "install: copying style %u failed at %lu", s, static_cast<unsigned long>(offset));
        return false;  // the header is still erased: the next load installs again
      }
      if (hooks.yield) hooks.yield();
    }
    ++block;
  }
  if (!store_.finishInstall()) return false;
  LOG_INF("TTF", "family installed: %u styles, log %lu B", in.styleCount,
          static_cast<unsigned long>(layout.geometry.logSize));
  return true;
}

bool TtfFont::configure(Size& size, const uint32_t ppem) {
  ttf::LineMetrics metrics[4];
  for (uint8_t s = 0; s < 4; ++s) {
    if (!styles_[s].present) continue;
    metrics[s] = ttf::lineMetrics(styles_[s].face, ppem);
    if (metrics[s].advanceY <= 0 || metrics[s].advanceY > 255) {
      LOG_ERR("TTF", "style %u at %lu/64 px: line height %ld does not fit", s, static_cast<unsigned long>(ppem),
              static_cast<long>(metrics[s].advanceY));
      return false;
    }
  }
  size.ppem = ppem;
  size.claimTried = false;
  for (uint8_t s = 0; s < 4; ++s) {
    Size::StyleFont& style = size.styles[s];
    style.provider.owner = this;
    style.provider.size = &size;
    style.provider.style = s;
    style.data = EpdFontData{};
    if (!styles_[s].present) continue;
    style.data.advanceY = static_cast<uint8_t>(metrics[s].advanceY);
    style.data.ascender = metrics[s].ascender;
    style.data.descender = metrics[s].descender;
    style.data.is2Bit = true;
    style.data.bitmap = store_.logBase();
    style.data.provider = &style.provider;
  }
  return true;
}

bool TtfFont::setBodySize(const uint8_t pointSize) {
  if (pointSize == 0 || pointSize > kMaxPointSize || !styles_[0].present) return false;
  if (!configure(body_, ttf::ppemFromPoints(pointSize))) return false;
  pointSize_ = pointSize;
  claimSize(body_);
  return true;
}

const EpdFont* TtfFont::font(const uint8_t style) const {
  return style < 4 && styles_[style].present && body_.ppem != 0 ? &body_.styles[style].font : nullptr;
}

TtfFont::Size* TtfFont::nativeSize(const uint8_t px) {
  if (px < kMinNativePx || px > maxNativePx_ || !styles_[0].present) return nullptr;
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
  if (!size) {
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
  for (uint8_t s = 0; s < 4; ++s) fonts[s] = styles_[s].present ? &size->styles[s].font : nullptr;
  if (servedPpem) *servedPpem = size->ppem;
  return true;
}

void TtfFont::dropNativeSizes() {
  for (auto& size : native_) size.reset();
  // No record is in use either (the same safe point): the ring's blocks, which a native size grew, go too.
  for (uint8_t i = 0; i < kRingSlots; ++i) freeRingSlot(i);
}

uint8_t TtfFont::nativeSizeCount() const {
  uint8_t count = 0;
  for (const auto& size : native_) count += size ? 1 : 0;
  return count;
}

bool TtfFont::claimSize(Size& size) {
  // A wipe voids every size index: claimed again once the store's generation moved. A claim the full table
  // refused is not asked again before then; that size's glyphs come from the ring.
  if (size.claimTried && size.generation == store_.generation()) return size.indexValid;
  size.claimTried = true;
  size.generation = store_.generation();
  size.indexValid = store_.sizeIndex(size.ppem, &size.index);
  return size.indexValid;
}

bool TtfFont::storeKey(Size& size, const uint8_t style, const uint32_t codepoint, uint32_t* key) {
  if (!store_.isOpen() || size.ppem == 0 || !claimSize(size)) return false;
  *key = glyphstore::makeKey(codepoint, style, size.index);
  return true;
}

bool TtfFont::fontBytes(const uint8_t style, FontBytes* bytes) {
  // Flash mode maps one style at a time, so the pointer is fetched again for every render.
  const uint8_t* mapped = store_.mapStyle(styles_[style].block);
  if (!mapped) {
    LOG_ERR("TTF", "style %u could not be mapped", style);
    return false;
  }
  *bytes = FontBytes(mapped, styles_[style].bytes);
  return true;
}

uint32_t TtfFont::batchCapacity() const { return rasterizer_.plan().outputBytes; }

const EpdGlyph* TtfFont::glyph(Size& size, const uint8_t style, const uint32_t codepoint) {
  if (style >= 4 || !styles_[style].present || size.ppem == 0) return nullptr;
  if (utf8IsDefaultIgnorable(codepoint)) return &kIgnorable;
  uint32_t key = 0;
  const bool haveKey = storeKey(size, style, codepoint, &key);
  if (haveKey) {
    const EpdGlyph* stored = nullptr;
    switch (store_.find(key, &stored)) {
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
  const ttf::Face& face = styles_[style].face;
  const uint16_t index = ttf::glyphIndex(bytes, face, codepoint);
  if (index == 0) {
    // The font has no glyph for it. An I/O error is not an answer (SD mode): ask again next time.
    if (haveKey && !ringMode_ && !bytes.ioError()) noteCommit(store_.markNoGlyph(key));
    return nullptr;
  }
  EpdGlyph metrics{};
  const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, index, size.ppem, &metrics), metrics);
  if (loaded != ttf::Status::Ok) {
    failed(style, codepoint, loaded, haveKey, key);
    return nullptr;
  }
  ++renders_;
  if (haveKey && !ringMode_ && glyphstore::recordBytes(metrics.dataLength) <= batchCapacity()) {
    // Drawn straight into a one-record batch in the output region, then stored.
    GlyphBatch batch(rasterizer_.output(), batchCapacity());
    uint8_t* bitmap = batch.add(key, metrics);
    if (bitmap) {
      const ttf::Status drawn = rasterizer_.rasterize(bitmap);
      if (drawn != ttf::Status::Ok) {
        batch.dropLast();
        failed(style, codepoint, drawn, haveKey, key);
        return nullptr;
      }
      const Commit committed = store_.commit(batch);
      const EpdGlyph* stored = nullptr;
      if (committed == Commit::Stored && store_.find(key, &stored) == Lookup::Found) return stored;
      noteCommit(committed);
    }
  }
  return drawToRing(metrics);
}

const EpdGlyph* TtfFont::drawToRing(const EpdGlyph& metrics) {
  const uint8_t slot = ringNext_;
  ringNext_ = static_cast<uint8_t>((ringNext_ + 1) % kRingSlots);
  if (metrics.dataLength > ringCapacity_[slot]) {
    // One heap block per slot, kept and reused while it is large enough: the ring exists only after the store
    // filled up, for oversized glyphs, for a size the store had no room for, or without a store. A record is
    // promised only until the next miss, so the oldest slots give way when the ring would pass its budget.
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

void TtfFont::freeRingSlot(const uint8_t slot) {
  ringBitmaps_[slot].reset();
  ringCapacity_[slot] = 0;
}

uint32_t TtfFont::ringBytes() const {
  uint32_t bytes = 0;
  for (const uint16_t capacity : ringCapacity_) bytes += capacity;
  return bytes;
}

bool TtfFont::ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
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

void TtfFont::failed(const uint8_t style, const uint32_t codepoint, const ttf::Status status, const bool haveKey,
                     const uint32_t key) {
  (void)style;  // both only logged, and logging may be compiled out
  (void)codepoint;
  // Transient (no scratch block, an SD read failed): drawn as missing this time, asked again at the next lookup.
  if (!ttf::isPermanent(status)) return;
  if (haveKey && !ringMode_) noteCommit(store_.markNoGlyph(key));
  if (failuresLogged_ < kMaxFailuresLogged) {
    ++failuresLogged_;
    LOG_INF("TTF", "U+%04lX style %u: %s, drawn as missing", static_cast<unsigned long>(codepoint), style,
            ttf::statusName(status));
  }
}

void TtfFont::noteCommit(const Commit result) {
  if (result == Commit::Full || result == Commit::WriteFailed) {
    if (!ringMode_) {
      LOG_INF("TTF", "glyph store %s: new glyphs are not stored until the next book open",
              result == Commit::Full ? "full" : "write failed");
    }
    ringMode_ = true;
  } else if (result == Commit::Refused) {
    LOG_ERR("TTF", "glyph store refused a write");
  }
}

void TtfFont::prefetch(Size& size, const uint8_t style, const char* utf8Text) {
  if (!utf8Text || style >= 4 || !styles_[style].present || ringMode_) return;
  uint32_t key = 0;
  if (!storeKey(size, style, 0, &key)) return;
  checkLock();
  FontBytes bytes;
  if (!fontBytes(style, &bytes)) return;
  const ttf::Face& face = styles_[style].face;

  // The text's codepoints that are not stored yet, each once.
  uint8_t count = 0;
  uint32_t asciiSeen[4] = {};
  const auto* text = reinterpret_cast<const unsigned char*>(utf8Text);
  while (count < kPrefetchGlyphs) {
    const uint32_t cp = utf8NextCodepoint(&text);
    if (cp == 0) break;
    if (cp < 0x80) {
      if (asciiSeen[cp >> 5] & (1u << (cp & 31))) continue;
      asciiSeen[cp >> 5] |= 1u << (cp & 31);
    }
    if (utf8IsDefaultIgnorable(cp)) continue;
    key = glyphstore::makeKey(cp, style, size.index);
    if (store_.find(key, nullptr) != Lookup::NotStored) continue;
    if (std::any_of(pending_, pending_ + count, [cp](const Pending& p) { return p.codepoint == cp; })) continue;
    const uint16_t index = ttf::glyphIndex(bytes, face, cp);
    if (index == 0) {
      if (!bytes.ioError()) noteCommit(store_.markNoGlyph(key));
      if (ringMode_) return;
      continue;
    }
    pending_[count++] = {cp, index};
  }
  if (count == 0) return;

  // In glyph order, which is file order: in SD mode the outline reads then move forward through the file.
  std::sort(pending_, pending_ + count, [](const Pending& a, const Pending& b) { return a.glyph < b.glyph; });
  GlyphBatch batch(rasterizer_.output(), batchCapacity());
  for (uint8_t i = 0; i < count && !ringMode_; ++i) {
    const Pending& p = pending_[i];
    key = glyphstore::makeKey(p.codepoint, style, size.index);
    EpdGlyph metrics{};
    const ttf::Status loaded = placeable(rasterizer_.load(bytes, face, p.glyph, size.ppem, &metrics), metrics);
    if (loaded != ttf::Status::Ok) {
      failed(style, p.codepoint, loaded, true, key);
      continue;
    }
    if (glyphstore::recordBytes(metrics.dataLength) > batchCapacity()) continue;  // drawn from the ring when used
    uint8_t* bitmap = batch.add(key, metrics);
    if (!bitmap) {
      noteCommit(store_.commit(batch));
      if (ringMode_) break;
      bitmap = batch.add(key, metrics);
      if (!bitmap) continue;
    }
    const ttf::Status drawn = rasterizer_.rasterize(bitmap);
    if (drawn != ttf::Status::Ok) {
      batch.dropLast();
      failed(style, p.codepoint, drawn, true, key);
      continue;
    }
    ++renders_;
    if (hooks.yield) hooks.yield();
  }
  if (!ringMode_) noteCommit(store_.commit(batch));
}

bool TtfFont::wipeIfPending(const std::function<void()>& onBusy) {
  if (!store_.isOpen() || !store_.wipePending()) return false;
  if (onBusy) onBusy();
  if (!store_.wipe()) return false;  // stays pending: the next book open tries again
  LOG_INF("TTF", "glyph store wiped");
  ringMode_ = false;
  for (uint8_t i = 0; i < kRingSlots; ++i) freeRingSlot(i);
  // The log may map elsewhere now; every size claims its index again at its next lookup.
  const auto rebase = [this](Size& size) {
    if (size.ppem == 0) return;
    for (uint8_t s = 0; s < 4; ++s) {
      if (styles_[s].present) size.styles[s].data.bitmap = store_.logBase();
    }
  };
  rebase(body_);
  for (auto& size : native_) {
    if (size) rebase(*size);
  }
  return true;
}
