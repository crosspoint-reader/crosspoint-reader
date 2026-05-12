#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  std::string textPool;
  textPool.reserve(512);
  extractWords(words, rows, textPool);
  mergeHyphenatedWords(words, rows, textPool);
  // Only consume the initial Confirm release if Confirm is still held at onEnter — i.e.
  // we were opened mid hold-to-lookup. Other entry paths (e.g. reader menu → Lookup) have
  // already released Confirm by the time we open, so consuming would swallow the user's
  // first deliberate tap and force them to press twice.
  const bool consumeInitialConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  navigator.load(std::move(words), std::move(rows), std::move(textPool), consumeInitialConfirm);
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows, std::string& textPool) {
  words.clear();
  words.reserve(64);
  rows.clear();
  rows.reserve(16);

  // Pre-build the SD-card font's persistent advance table for this page's
  // text. Without this, getTextAdvanceX falls through to a slow fontMap
  // path that triggers SD reads for every measurement (~50ms each); with
  // this, all subsequent advance-X calls take the in-RAM binary-search
  // fast path. One batched SD operation here replaces ~30 individual
  // cold-cache lookups in the per-block first-word / last-word fallback
  // measurements below.
  //
  // The advance table persists across clearCache() (per SdCardFont.h:201)
  // so this only pays the SD cost the first time the page is opened in
  // word-select; subsequent re-entries amortize it.
  //
  // Memory: pageText reservation is ~2KB transient (same pattern as
  // FontCacheManager::PrewarmScope's scanText_), freed when extractWords
  // returns.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t pageStyleMask = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    for (size_t i = 0; i < words.size(); i++) {
      pageText.append(words[i]);
      pageText.push_back(' ');
      if (i < styles.size()) {
        pageStyleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(styles[i]) & 0x03));
      }
    }
  }
  if (pageStyleMask == 0) pageStyleMask = 1u << EpdFontFamily::REGULAR;
  renderer.ensureSdCardFontReady(SETTINGS.getReaderFontId(), pageText.c_str(), pageStyleMask);

  // Natural inter-word space at the regular style — fallback for blocks
  // where we can't derive a per-line gap (single-word blocks, degenerate
  // first-word measurements).
  const int16_t naturalSpaceWidth =
      static_cast<int16_t>(renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), " ", EpdFontFamily::REGULAR));

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    const auto& wordList = block->getWords();
    const auto& xPosList = block->getWordXpos();
    const auto& styleList = block->getWordStyles();

    // Derive per-line inter-word gap by measuring the first word's actual
    // width and subtracting from xPos[1] - xPos[0]. Justified blocks
    // stretch the natural space (ParsedText.cpp:514-553 adds justifyExtra
    // per gap), so a single global space width leaves residual trailing
    // gap on justified lines. One advance-X lookup per block — backed by
    // the persistent advance table, not SD glyph loads — yields per-line
    // gaps cheaply.
    int16_t lineGapWidth = naturalSpaceWidth;
    if (wordList.size() >= 2 && xPosList.size() >= 2 && !wordList[0].empty()) {
      const EpdFontFamily::Style firstStyle = (!styleList.empty()) ? styleList[0] : EpdFontFamily::REGULAR;
      // Use advance-X (persistent advance table, survives clearCache()) rather
      // than getTextWidth, which loads glyph bitmaps from SD per cold codepoint
      // and thrashes the 8-slot overflow ring. Layout itself uses advance-X
      // (ParsedText.cpp:64,74) so the derived gap is exact, not approximate.
      const int16_t firstWidth =
          static_cast<int16_t>(renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), wordList[0].c_str(), firstStyle));
      const int16_t derivedGap = static_cast<int16_t>(xPosList[1] - xPosList[0] - firstWidth);
      if (derivedGap > 0) lineGapWidth = derivedGap;
    }

    auto wordIt = wordList.begin();
    auto xIt = xPosList.begin();
    auto styleIt = styleList.begin();

    while (wordIt != wordList.end() && xIt != xPosList.end()) {
      int16_t screenX = line->xPos + static_cast<int16_t>(*xIt) + marginLeft;
      int16_t screenY = line->yPos + marginTop;
      const std::string& wordText = *wordIt;
      const EpdFontFamily::Style wordStyle = (styleIt != styleList.end()) ? *styleIt : EpdFontFamily::REGULAR;

      // Skip tokens with no alphanumeric characters (bullets, punctuation, etc.)
      if (!std::any_of(wordText.begin(), wordText.end(), [](unsigned char c) { return std::isalnum(c); })) {
        ++wordIt;
        ++xIt;
        if (styleIt != styleList.end()) ++styleIt;
        continue;
      }

      // Split on en-dash (U+2013: E2 80 93) and em-dash (U+2014: E2 80 94)
      std::vector<size_t> splitStarts;
      splitStarts.reserve(4);
      size_t partStart = 0;
      for (size_t i = 0; i < wordText.size();) {
        if (i + 2 < wordText.size() && static_cast<uint8_t>(wordText[i]) == 0xE2 &&
            static_cast<uint8_t>(wordText[i + 1]) == 0x80 &&
            (static_cast<uint8_t>(wordText[i + 2]) == 0x93 || static_cast<uint8_t>(wordText[i + 2]) == 0x94)) {
          if (i > partStart) splitStarts.push_back(partStart);
          i += 3;
          partStart = i;
        } else {
          i++;
        }
      }
      if (partStart < wordText.size()) splitStarts.push_back(partStart);

      if (splitStarts.size() <= 1 && partStart == 0) {
        // Derive width from the already-laid-out next-word xpos. This avoids
        // per-word getTextWidth() calls, which on SD-card fonts trigger a
        // glyph fetch per missing codepoint and thrash the 8-slot overflow
        // ring (OVERFLOW_CAPACITY in SdCardFont.h). With this derivation,
        // measurements drop from ~100-200 per page to ~10-20 (one per
        // line — the last word per block, handled below).
        //
        // The raw xpos diff includes the trailing inter-word gap; subtract
        // lineGapWidth (derived per-block from the first word, accounting
        // for justified-line stretching) so the highlight rectangle hugs
        // the visible glyphs rather than extending into the gap before
        // the next word.
        //
        // Punctuation tokens skipped above leave their xpos entries in
        // xPosList as boundary markers; the diff to the next iterator
        // position correctly covers the visual run regardless of whether
        // the next token is a word or a punctuation token.
        //
        // Continuation-word negative kerning (see ParsedText layout), or
        // short words like "I" where word + space ~= spaceWidth, can make
        // the post-subtraction width zero or negative; clamp to a small
        // positive floor so downstream consumers (highlight fillRect,
        // findClosestWord centerX) stay well-formed.
        //
        // Last word per block has no next xpos — fall back to advance-X
        // (also cheap, backed by the persistent advance table).
        int16_t wordWidth;
        const auto nextXIt = xIt + 1;
        if (nextXIt != xPosList.end()) {
          const int16_t raw = static_cast<int16_t>(*nextXIt - *xIt);
          wordWidth = std::max(static_cast<int16_t>(1), static_cast<int16_t>(raw - lineGapWidth));
        } else {
          // Advance-X (persistent advance table) instead of getTextWidth to
          // avoid SD glyph-bitmap loads. Matches layout's own measurement.
          wordWidth = renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), wordText.c_str(), wordStyle);
        }
        {
          uint16_t off = WordSelectNavigator::poolAppend(textPool, wordText.c_str(), wordText.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = off;
          wi.textLen = static_cast<uint16_t>(wordText.size());
          wi.lookupOffset = off;
          wi.lookupLen = wi.textLen;
          wi.screenX = screenX;
          wi.screenY = screenY;
          wi.width = wordWidth;
          wi.style = wordStyle;
          wi.fontId = SETTINGS.getReaderFontId();
          words.push_back(wi);
        }
      } else {
        for (size_t si = 0; si < splitStarts.size(); si++) {
          size_t start = splitStarts[si];
          size_t end = (si + 1 < splitStarts.size()) ? splitStarts[si + 1] : wordText.size();
          size_t textEnd = end;
          while (textEnd > start && textEnd <= wordText.size()) {
            if (textEnd >= 3 && static_cast<uint8_t>(wordText[textEnd - 3]) == 0xE2 &&
                static_cast<uint8_t>(wordText[textEnd - 2]) == 0x80 &&
                (static_cast<uint8_t>(wordText[textEnd - 1]) == 0x93 ||
                 static_cast<uint8_t>(wordText[textEnd - 1]) == 0x94)) {
              textEnd -= 3;
            } else {
              break;
            }
          }
          std::string part = wordText.substr(start, textEnd - start);
          if (part.empty()) continue;

          std::string prefix = wordText.substr(0, start);
          // Advance-X consistent with the rest of extractWords and with layout.
          int16_t offsetX =
              prefix.empty() ? 0 : renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), prefix.c_str(), wordStyle);
          int16_t partWidth = renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), part.c_str(), wordStyle);
          {
            uint16_t off = WordSelectNavigator::poolAppend(textPool, part.c_str(), part.size());
            WordSelectNavigator::WordInfo wi;
            wi.textOffset = off;
            wi.textLen = static_cast<uint16_t>(part.size());
            wi.lookupOffset = off;
            wi.lookupLen = wi.textLen;
            wi.screenX = static_cast<int16_t>(screenX + offsetX);
            wi.screenY = screenY;
            wi.width = partWidth;
            wi.style = wordStyle;
            wi.fontId = SETTINGS.getReaderFontId();
            words.push_back(wi);
          }
        }
      }

      ++wordIt;
      ++xIt;
      if (styleIt != styleList.end()) ++styleIt;
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
}

void DictionaryWordSelectActivity::mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                        std::vector<WordSelectNavigator::Row>& rows,
                                                        std::string& textPool) {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordIndices.empty() || rows[r + 1].wordIndices.empty()) continue;

    int lastWordIdx = rows[r].wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen == 0) continue;

    if (!utf8EndsWithHyphen(lastWord, lastLen)) continue;

    int nextWordIdx = rows[r + 1].wordIndices.front();
    words[lastWordIdx].continuationIndex = nextWordIdx;
    words[nextWordIdx].continuationOf = lastWordIdx;

    std::string firstPart(lastWord, lastLen);
    utf8RemoveTrailingHyphen(firstPart);
    const char* nextWord = textPool.data() + words[nextWordIdx].textOffset;
    std::string merged = firstPart + nextWord;
    uint16_t mergedOff = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
    words[lastWordIdx].lookupOffset = mergedOff;
    words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].lookupOffset = mergedOff;
    words[nextWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].continuationIndex = nextWordIdx;
  }

  // Cross-page hyphenation
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen > 0 && utf8EndsWithHyphen(lastWord, lastLen)) {
      std::string firstPart(lastWord, lastLen);
      utf8RemoveTrailingHyphen(firstPart);
      std::string merged = firstPart + nextPageFirstWord;
      uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
      words[lastWordIdx].lookupOffset = off;
      words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    }
  }

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());
}

void DictionaryWordSelectActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(),
                                   true, cachePath, controller.getRecordHistory(), controller.getLookupWord(),
                                   DictionaryLookupController::toHistStatus(controller.getFoundStatus())),
                               [this](const ActivityResult& result) {
                                 if (!result.isCancelled) {
                                   setResult(ActivityResult{});
                                   finish();
                                 } else {
                                   forceFullRepaintOnNextRender();
                                   requestUpdate();
                                 }
                               });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  if (navigator.isEmpty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (navigator.handleNavigation(mappedInput, renderer)) {
    requestUpdate();
  }

  // Check Back early when not in multi-select mode. This allows exit even when
  // confirmReleaseConsumed is stuck true (menu-triggered entry has no Confirm release).
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  if (controller.handleMultiSelect(navigator)) return;

  if (navigator.isMultiSelecting()) return;

  if (controller.handleConfirmLookup(navigator)) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  const int currIdx = navigator.getCurrentFlatIndex();

  // Differential fast path. Only valid when:
  //   - we set it up on the previous frame (RenderMode::Differential),
  //   - the controller has nothing pending to draw,
  //   - we have a current selection.
  if (nextRenderMode_ == RenderMode::Differential && !controller.isActive() && currIdx >= 0) {
    // Prewarm bitmap glyphs for the word about to be highlighted: one
    // batched SD operation instead of N cold-miss loads serialized through
    // drawText. Saves ~25-90ms for typical word lengths; words with >8
    // unique chars partly thrash the 8-slot overflow ring (no benefit but
    // also no regression vs. uncached drawText).
    if (const auto* w = navigator.getWordAt(currIdx)) {
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w),
                          static_cast<uint8_t>(1u << (static_cast<uint8_t>(w->style) & 0x03)));
      }
    }
    auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx);
    if (dirty.has_value()) {
      // Differential framebuffer modifications (snapshot restore / capture / draw) are correct,
      // but the SDK's "EXPERIMENTAL" displayWindow path produces alternating black→white
      // transition failures on consecutive fast partial refreshes. Use the full displayBuffer
      // here instead — we still save the expensive page->render call, which is the bulk of the
      // pre-optimization cost.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      return;
    }
    // Fall through to full repaint.
  }

  // Skip-initial-render fast path. Fires at most once per activity instance,
  // when the caller signalled the framebuffer already contains the page at
  // our margins (currently only EpubReaderActivity's hold-to-lookup path).
  // Conditions:
  //   - flag still set (one-shot),
  //   - controller has nothing to draw (an active controller would mean we
  //     re-entered render() after a sub-activity returned without the
  //     framebuffer being reset by forceFullRepaintOnNextRender()),
  //   - we have a current selection (currIdx >= 0); otherwise there is
  //     nothing to overlay and we fall through to a normal repaint.
  // We consume the flag unconditionally on first entry so any later
  // full-repaint goes through the normal clearScreen + page->render path.
  if (framebufferContainsPage_) {
    framebufferContainsPage_ = false;
    if (!controller.isActive() && currIdx >= 0) {
      // Clear the bottom status-bar strip that EpubReader rendered (chapter,
      // page numbers, battery, progress bar). The menu→lookup path naturally
      // wipes this via clearScreen() + page->render(); the fast path skipped
      // both, so we explicitly clear only that reserved region to match.
      // The geometry mirrors EpubReaderActivity's orientedMarginBottom
      // calculation (bezel + max(screenMargin, statusBarHeight)).
      int bezelTop, bezelRight, bezelBottom, bezelLeft;
      renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
      const int reservedHeight =
          std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getInstance().getStatusBarHeight());
      const int clearY = renderer.getScreenHeight() - bezelBottom - reservedHeight;
      const int clearW = renderer.getScreenWidth() - bezelLeft - bezelRight;
      renderer.clearRect(bezelLeft, clearY, clearW, reservedHeight);

      // See the matching block in the differential fast path above — batched
      // glyph prewarm avoids per-character cold-miss SD reads during the
      // upcoming drawText for the selected word.
      if (const auto* w = navigator.getWordAt(currIdx)) {
        if (auto* fcm = renderer.getFontCacheManager()) {
          fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w),
                            static_cast<uint8_t>(1u << (static_cast<uint8_t>(w->style) & 0x03)));
        }
      }

      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
      bool snapshotPrimed = setup.has_value();
      if (!snapshotPrimed) {
        // Hyphenated wrap or oversize capture. The framebuffer still holds
        // the page, but we cannot prime the snapshot for the differential
        // path. Draw the multi-word highlight (which overwrites pixels under
        // each highlight rect) and force the next render to do a full
        // repaint so the renderer state is consistent. The user just pays
        // for one regular page render on the next cursor move instead of
        // on entry.
        navigator.renderHighlight(renderer, lineHeight);
      }
      const auto labels = mappedInput.mapLabels("", "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
      return;
    }
    // Flag was set but conditions weren't met (controller active or no
    // current selection). Fall through to the normal full-repaint path.
  }

  // Full repaint path.
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // Without this, every cold codepoint cold-misses the 8-slot SD glyph
  // overflow ring and the page render serializes ~100+ individual SD reads.
  // Same pattern as EpubReaderActivity::renderContents().
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);  // scan pass
  scope.endScanAndPrewarm();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);

  // Set up snapshot AND draw the highlight via the differential entry point with
  // prevWordIdx = -1 (no previous highlight to wipe). This both draws the highlight
  // for this frame and primes snapshot_ so the next frame can run the fast path.
  // If the navigator declines (multi-select, hyphenated, oversize), fall back to
  // the multi-word renderHighlight and stay on the full path next frame.
  //
  // The -1 literal is load-bearing: renderHighlightDifferential uses prevWordIdx
  // < 0 as the signal "framebuffer was just redrawn from scratch, discard any
  // stale snapshot rather than restoring it on top of fresh pixels." This is the
  // only path that disturbs the framebuffer outside the differential cycle, so
  // it's also the only call site that must pass -1.
  bool snapshotPrimed = false;
  if (currIdx >= 0) {
    auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
    snapshotPrimed = setup.has_value();
  }
  if (!snapshotPrimed) {
    navigator.renderHighlight(renderer, lineHeight);
  }

  const auto labels = mappedInput.mapLabels("", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  prevHighlightIdx_ = currIdx;
  nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
}
