#include "FontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontRegistry.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "FontPreviewCache.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

void FontSelectionActivity::onEnter() {
  const int fontCount = static_cast<int>(fontFamilyOptionCount()) + (overrideSetting ? 1 : 0);
  int selectedIndex = overrideSetting ? static_cast<int>(overrideSetting->getEnumSelectedIndex())
                                      : static_cast<int>(target == Target::TXT ? txtFontFamilyDynamicGetter(nullptr)
                                                                               : fontFamilyDynamicGetter(nullptr));
  if (selectedIndex >= fontCount) selectedIndex = 0;

  rowLabels.clear();
  rowItems.clear();
  rowLabels.reserve(fontCount);
  rowItems.reserve(fontCount);
  for (int index = 0; index < fontCount; ++index) {
    rowLabels.push_back(overrideSetting ? overrideSetting->getEnumOptionLabel(static_cast<uint8_t>(index))
                                        : fontFamilyOptionLabel(static_cast<uint8_t>(index)));
  }
  for (int index = 0; index < fontCount; ++index) {
    fui::ListItem item;
    item.label = rowLabels[index].c_str();
    if (index == selectedIndex) item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(index);
    rowItems.push_back(item);
  }

  RenderLock lock(*this);
  lendPreviewArena();
  UiListActivity::onEnter();
  nav.selected = selectedIndex;
  updatePreviewFontLocked(selectedIndex);
}

void FontSelectionActivity::onExit() {
  // Nothing may be drawn before the return: the exit's busy indicator may still be finishing, and
  // with the buffer lent its finish reads the write buffer (see returnPreviewArena).
  dropPreviewFont();
  returnPreviewArena();
  UiListActivity::onExit();
}

void FontSelectionActivity::lendPreviewArena() {
  if (previewArena_ || !renderer.hasSecondaryBuffer()) return;
  // HomeActivity's lend sequence. Safe with the transition's busy-indicator waveform still running:
  // the copy only reads the displayed frame, and the RED seed and the borrow both drain it first.
  renderer.syncWriteBufferFromDisplayed();
  renderer.syncRedRamFromFrameBuffer();
  size_t lentSize = 0;
  uint8_t* lent = renderer.borrowSecondaryBuffer(&lentSize);
  if (!lent) return;
  previewArena_ = makeUniqueNoThrow<BuildArena>(lent, lentSize);
  if (!previewArena_ || !previewArena_->valid()) {
    previewArena_.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    return;
  }
  // The selector draws only plain black-and-white frames, so FAST keeps diffing against the seeded
  // RED baseline instead of degrading to HALF on the X4. No-op on X3.
  renderer.setSingleBufferFastDiff(true);
  LOG_DBG("FPRV", "Lent secondary framebuffer for previews (%u bytes)", static_cast<unsigned>(lentSize));
}

void FontSelectionActivity::returnPreviewArena() {
  if (!previewArena_) return;
  previewArena_.reset();
  renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  renderer.setSingleBufferFastDiff(false);
}

void FontSelectionActivity::loadPreviewFont(const SdCardFontFamilyInfo& family) {
  dropPreviewFont();
  if (previewArena_) {
    previewBlock_ = previewArena_->reserveBlock();
    sdFontSystem.ensureLoadedForPreview(renderer, family.name.c_str(), selectedFontSize(), previewArena_.get());
    if (sdFontSystem.resolveFontId(family.name.c_str(), selectedFontSize()) != 0) return;
    LOG_ERR("FPRV", "%s did not load into the lent buffer (%u of %u bytes used); trying the heap", family.name.c_str(),
            static_cast<unsigned>(previewArena_->used()), static_cast<unsigned>(previewArena_->capacity()));
    dropPreviewFont();
  }
  sdFontSystem.ensureLoadedForPreview(renderer, family.name.c_str(), selectedFontSize());
}

void FontSelectionActivity::dropPreviewFont() {
  sdFontSystem.unload(renderer);
  // Only after the unload: the font's arrays live in this block (SdCardFont::useArena).
  if (previewArena_ && previewBlock_.valid()) previewArena_->release(previewBlock_);
}

const char* FontSelectionActivity::headerTitle() const {
  if (target == Target::TXT) return tr(STR_TXT_FONT_FAMILY);
  return tr(STR_FONT_FAMILY);
}

uint8_t FontSelectionActivity::selectedFontSize() const {
  return target == Target::TXT ? SETTINGS.txtFontPointSize : SETTINGS.fontPointSize;
}

int FontSelectionActivity::previewOptionIndex(const int index) const {
  if (!overrideSetting) return index;
  if (index == 0) return static_cast<int>(fontFamilyDynamicGetter(nullptr));
  return index - 1;
}

void FontSelectionActivity::updatePreviewFont(const int index) {
  if (index < 0 || index >= listCount()) return;

  RenderLock lock(*this);
  updatePreviewFontLocked(index);
}

bool FontSelectionActivity::previewKey(FontPreviewCache::Key& key) const {
  if (previewFamily.empty() || previewW <= 0 || previewH <= 0) return false;
  key.familyName = previewFamily.c_str();
  key.sourceSize = previewSourceBytes;
  key.width = static_cast<uint16_t>(previewW);
  key.height = static_cast<uint16_t>(previewH);
  key.pointSize = previewPointSize;
  key.language = static_cast<uint8_t>(I18N.getLanguage());
  return true;
}

int FontSelectionActivity::prepareSdPreview(const SdCardFontFamilyInfo& family) {
  const uint8_t targetPt = sdFontSystem.sizeListFor(family.name.c_str()).snap(selectedFontSize());
  const SdCardFontFileInfo* file = family.pickClosestSize(targetPt);
  if (!file) {
    previewFamily.clear();
    return 0;
  }

  previewFamily = family.name;
  // The size DRAWN, not the size of the file: a family lacking the selected size renders its
  // closest face scaled (SdCardFontManager::ensureSizeAlias), so 22 pt and 24 pt from the same
  // 18 pt file are different pictures and must not share a cache entry.
  previewPointSize = targetPt;
  previewSourceBytes = file->fileBytes;

  FontPreviewCache::Key key;
  if (previewKey(key) && FontPreviewCache::available(key)) {
    // The pixels are on the card, so nothing has to be resident to draw them.
    // Dropping the font here is what keeps browsing flat on the heap.
    dropPreviewFont();
    previewFromCache = true;
    return 0;
  }

  // Preview-only: never let browsing the list rewrite the flash font partition.
  // The reader's own ensureLoaded() caches whatever the user actually picks
  // when the book opens.
  loadPreviewFont(family);
  const int fontId = sdFontSystem.resolveFontId(family.name.c_str(), selectedFontSize());
  if (fontId != 0) prewarmPreviewGlyphs(fontId);
  previewNeedsStore = fontId != 0;
  return fontId;
}

// Without this the preview render thrashes: an SD font keeps only 8 overflow
// glyph slots, the sample sentence needs about twice that, and layout walks the
// text several times -- so every glyph is evicted and re-read from SD on each
// pass, at ~12 ms a go. Measured on device: >1280 misses and 15+ seconds for one
// preview. FontCacheManager.h states the rule outright ("prewarm every font it
// uses, or the render thrashes the glyph cache"); this screen was simply never
// following it.
//
// Only the regular weight is warmed. A style costs a glyph arena plus its own
// kern/ligature tables, and warming two left 1,624 bytes of free heap at the
// low-water mark on an X3 with six SD families.
void FontSelectionActivity::prewarmPreviewGlyphs(const int fontId) const {
  auto* fontCache = renderer.getFontCacheManager();
  if (!fontCache) return;
  // prewarmCache() appends without clearing, so previewing font after font would
  // otherwise pile up a page slot per font.
  fontCache->clearCache();
  fontCache->prewarmCache(fontId, tr(STR_FONT_PREVIEW_SAMPLE), PREVIEW_STYLE_MASK);
}

void FontSelectionActivity::updatePreviewFontLocked(const int index) {
  const int optionIndex = previewOptionIndex(index);
  const uint32_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const uint32_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  previewFromCache = false;
  previewNeedsStore = false;
  int previewFontId = 0;
  if (optionIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    previewFamily.clear();
    dropPreviewFont();
    previewFontId =
        CrossPointSettings::getBuiltinReaderFontIdForPoints(static_cast<uint8_t>(optionIndex), selectedFontSize());
  } else {
    const auto& families = sdFontSystem.registry().getFamilies();
    const size_t familyIndex = static_cast<size_t>(optionIndex - CrossPointSettings::BUILTIN_FONT_COUNT);
    if (familyIndex < families.size()) {
      previewFontId = prepareSdPreview(families[familyIndex]);
    } else {
      previewFamily.clear();
    }
  }
  // Also the fallback when the strip is blitted: FONT_TITLE must never name a
  // font that is not loaded, even though nothing draws with it in that case.
  if (previewFontId == 0) {
    previewFontId =
        CrossPointSettings::getBuiltinReaderFontIdForPoints(CrossPointSettings::NOTOSANS, selectedFontSize());
  }
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, previewFontId);

  LOG_DBG("FPRV", "font=%d free=%lu->%lu largest=%lu->%lu", index, static_cast<unsigned long>(freeBefore),
          static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
          static_cast<unsigned long>(largestBefore),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
}

void FontSelectionActivity::onSelectionChanged(const int index) { updatePreviewFont(index); }

void FontSelectionActivity::afterUiRender() {
  handlePreviewStrip();
  if (warmupActive) drawWarmupNotice();
  drawPreviewFrame();
}

// Drawn after the strip is captured or blitted, so it is chrome rather than
// cached pixels: the frame then costs nothing in the cache and stays correct if
// the theme changes under an already-stored strip.
void FontSelectionActivity::drawPreviewFrame() const {
  if (previewW <= 0 || previewH <= 0) return;
  renderer.drawRect(previewX, previewY, previewW, previewH, 1, true);
}

// The warm-up has to render each family into the preview rectangle to capture a
// strip of it, but showing those renders made the preview cycle through fonts
// unrelated to the cursor. The capture has already happened by the time this
// runs, so the rectangle can be painted over with the progress message -- which
// is where the eye already is, and it keeps the footer free for the Back hint.
void FontSelectionActivity::drawWarmupNotice() const {
  if (previewW <= 0 || previewH <= 0) return;
  renderer.fillRect(previewX, previewY, previewW, previewH, false);

  char buf[64];
  // warmupDone counts completed items, so it is 0 on the frame before the first
  // load; show that frame as "1 of N" rather than "0 of N".
  const unsigned shown = warmupDone < 1 ? 1u : static_cast<unsigned>(warmupDone);
  snprintf(buf, sizeof(buf), tr(STR_CREATING_FONT_PREVIEWS), shown, static_cast<unsigned>(warmupQueue.size()));
  const int textHeight = renderer.getTextHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, previewY + (previewH - textHeight) / 2, buf);
}

void FontSelectionActivity::handlePreviewStrip() {
  FontPreviewCache::Key key;
  if (!previewKey(key)) return;  // a built-in font, or the rectangle is not known yet

  if (previewFromCache) {
    if (FontPreviewCache::restore(renderer, key, previewX, previewY)) return;
    // The entry stopped matching between the probe and the blit -- the card was
    // pulled, or the font was replaced underneath us. Load the font and render
    // properly next pass rather than leaving a half-painted strip.
    LOG_DBG("FPRV", "Cached strip for %s unusable; re-rendering", key.familyName);
    previewFromCache = false;
    updatePreviewFontLocked(nav.selected);
    requestUpdate();
    return;
  }

  if (previewNeedsStore) {
    previewNeedsStore = false;
    FontPreviewCache::store(renderer, key, previewX, previewY);
  }
}

void FontSelectionActivity::scanForMissingPreviews() {
  const auto& families = sdFontSystem.registry().getFamilies();
  if (families.empty() || previewW <= 0 || previewH <= 0) return;

  const auto language = static_cast<uint8_t>(I18N.getLanguage());

  // The override variant lists the inherited font as row 0 as well as in its own
  // row, so the same family can appear twice. Warming it twice would cost a
  // whole font load for nothing.
  std::vector<int> seen;
  seen.reserve(families.size());
  warmupQueue.reserve(families.size());

  for (int row = 0; row < listCount(); ++row) {
    const int optionIndex = previewOptionIndex(row);
    if (optionIndex < CrossPointSettings::BUILTIN_FONT_COUNT) continue;  // built-ins live in flash
    const auto familyIndex = static_cast<size_t>(optionIndex - CrossPointSettings::BUILTIN_FONT_COUNT);
    if (familyIndex >= families.size()) continue;
    if (std::find(seen.begin(), seen.end(), optionIndex) != seen.end()) continue;
    seen.push_back(optionIndex);

    const auto& family = families[familyIndex];
    // Per family: each draws the selected size snapped to its own list, and that is the size
    // prepareSdPreview() keys the preview on.
    const uint8_t targetPt = sdFontSystem.sizeListFor(family.name.c_str()).snap(selectedFontSize());
    const SdCardFontFileInfo* file = family.pickClosestSize(targetPt);
    if (!file) continue;

    FontPreviewCache::Key key;
    key.familyName = family.name.c_str();
    key.sourceSize = file->fileBytes;
    key.width = static_cast<uint16_t>(previewW);
    key.height = static_cast<uint16_t>(previewH);
    // The size DRAWN, as prepareSdPreview() stores it. Keyed on the file's size, a family drawn
    // scaled (22 pt from an 18 pt file) was never found, and every visit ran the warm-up again.
    key.pointSize = targetPt;
    key.language = language;
    if (!FontPreviewCache::available(key)) warmupQueue.push_back(row);
  }

  LOG_DBG("FPRV", "%u of %d rows need a preview", static_cast<unsigned>(warmupQueue.size()), listCount());
}

// Leaving the warm-up has to be atomic against the render task. Clearing
// warmupActive and then calling updatePreviewFont() -- which only takes the lock
// once it is already inside -- left a window where a render pass saw
// warmupActive == false while FONT_TITLE still pointed at the last warmed font.
// That pass skipped the progress notice and painted the wrong font's sample for
// one frame before the correct one arrived.
void FontSelectionActivity::finishWarmup() {
  const int selected = nav.selected;
  {
    RenderLock lock(*this);
    warmupActive = false;
    warmupQueue.clear();
    warmupDone = 0;
    if (selected >= 0 && selected < listCount()) updatePreviewFontLocked(selected);
  }
  requestUpdate();
}

void FontSelectionActivity::advanceWarmup() {
  // The strip for the row in hand has not reached the framebuffer yet; a render
  // has to happen between two items or the first one is never captured.
  if (previewNeedsStore) return;

  if (warmupDone >= warmupQueue.size()) {
    finishWarmup();  // back to whatever the cursor is on
    return;
  }

  // Drop the family in hand BEFORE measuring. The next load unloads it anyway,
  // so the heap it occupies is not pressure on that load -- measuring while it
  // was still resident read the font's own footprint as exhaustion and stopped
  // the warm-up after one family (largest 22516 -> 8692, then "below floor").
  // With nothing resident this reads ~22.5 KB on an X3 against the ~15 KB a
  // kern-heavy family needs, so the brake only bites when something else has
  // genuinely taken the big block.
  dropPreviewFont();
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  // The brake only matters on the heap path: with the buffer lent, previews never touch the heap.
  if (!previewArena_ && largest < WARMUP_MIN_CONTIGUOUS) {
    LOG_ERR("FPRV", "Stopping warm-up at %u/%u: largest free block %lu below floor", static_cast<unsigned>(warmupDone),
            static_cast<unsigned>(warmupQueue.size()), static_cast<unsigned long>(largest));
    finishWarmup();
    return;
  }

  updatePreviewFont(warmupQueue[warmupDone++]);
  requestUpdate();
}

bool FontSelectionActivity::handleCustomInput() {
  // Deferred to the first loop() tick rather than done in afterUiRender(), which
  // runs before displayBuffer(): the scan costs an open per family, and doing it
  // there held the list off the screen for as long as it took.
  if (!warmupScanned && previewW > 0 && previewH > 0) {
    warmupScanned = true;
    scanForMissingPreviews();
    warmupActive = !warmupQueue.empty();
    if (warmupActive) {
      requestUpdate();  // repaint with the progress line before the first load
      return true;
    }
  }

  if (!warmupActive) return false;
  // Back abandons the warm-up; the remaining families then build lazily, one per cursor move,
  // exactly as they would without it. Read as an event like every other press on this list: a
  // Back read here by level would reach the list afterwards as an event and close the screen.
  // Other presses are dropped while the warm-up owns the screen.
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) {
      LOG_DBG("FPRV", "Warm-up cancelled after %u of %u", static_cast<unsigned>(warmupDone),
              static_cast<unsigned>(warmupQueue.size()));
      finishWarmup();
      return true;
    }
  }
  advanceWarmup();
  return true;  // the warm-up owns the screen until it finishes
}

void FontSelectionActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  if (overrideSetting) {
    overrideSetting->setEnumSelectedIndex(static_cast<uint8_t>(index));
  } else if (target == Target::TXT) {
    txtFontFamilyDynamicSetter(nullptr, static_cast<uint8_t>(index));
  } else {
    fontFamilyDynamicSetter(nullptr, static_cast<uint8_t>(index));
  }
  finish();
}

void FontSelectionActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);

  // The preview rectangle is only knowable here, and afterUiRender() needs it
  // to blit or capture the strip.
  const fui::Rect bodyRect = screen.body();
  const auto previewHeight = static_cast<int16_t>(bodyRect.height * 30 / 100);
  previewX = bodyRect.x;
  previewY = bodyRect.y;
  previewW = bodyRect.width;
  previewH = previewHeight;

  if (previewFromCache) {
    // afterUiRender() blits the strip over this space. Drawing sample text here
    // would only be overwritten, and its font is deliberately not loaded.
    screen.spacer(previewHeight);
  } else {
    fui::TextAreaProps preview;
    preview.text = tr(STR_FONT_PREVIEW_SAMPLE);
    preview.style = screen.theme().titleText;
    preview.style.font = fui::GfxRendererTarget::FONT_TITLE;
    // Regular weight, deliberately, whatever the theme's title style says: each
    // warmed style costs its own glyph arena AND its own kern/ligature tables
    // (Merriweather loaded kernL=841/kernR=870 twice when both were warmed).
    // Fixing the weight also makes PREVIEW_STYLE_MASK a constant, so the first
    // font no longer has to warm both to be safe.
    preview.style.bold = false;
    preview.showCaret = false;
    screen.textArea(preview, previewHeight);
  }
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));

  auto props = listProps(screen);
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  addList(screen, props);
}
