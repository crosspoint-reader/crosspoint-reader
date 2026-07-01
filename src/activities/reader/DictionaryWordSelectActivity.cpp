#include "DictionaryWordSelectActivity.h"

#include <Epub/FocusReading.h>
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
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"

namespace {

// Soft-hyphen U+00AD encoded as 2 UTF-8 bytes. Layout (ParsedText.cpp:19)
// strips these before measurement, so we mirror that here — otherwise
// derived word widths include the soft-hyphen glyph's advance and the
// highlight rectangle overruns into the inter-word gap.
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

int16_t measureWordAdvanceX(const GfxRenderer& renderer, int fontId, const std::string& word,
                            EpdFontFamily::Style style, const FocusReading::Annotation* focus = nullptr) {
  if (word.find(SOFT_HYPHEN_UTF8) == std::string::npos) {
    return static_cast<int16_t>(
        FocusReading::getTextAdvanceX(renderer, fontId, word.c_str(), style, SETTINGS.focusReadingEnabled, focus));
  }
  std::string sanitized = word;
  size_t pos = 0;
  while ((pos = sanitized.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    sanitized.erase(pos, SOFT_HYPHEN_BYTES);
  }
  return static_cast<int16_t>(
      FocusReading::getTextAdvanceX(renderer, fontId, sanitized.c_str(), style, SETTINGS.focusReadingEnabled, focus));
}

// Single-style prewarm/advance-table bitmask: bit 0 = REGULAR, 1 = BOLD,
// 2 = ITALIC, 3 = BOLD_ITALIC. The `& 0x03` is defensive — Style enum
// is two bits, but UNDERLINE etc. live in higher bits if ever OR'd in.
constexpr uint8_t styleToBitMask(EpdFontFamily::Style style) {
  return static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
}

}  // namespace

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
  navigator.load(std::move(words), std::move(rows), std::move(textPool), consumeInitialConfirm,
                 renderer.getScreenWidth() / 2);
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void DictionaryWordSelectActivity::prewarmHighlightGlyphs(int currIdx) {
  const auto* w = navigator.getWordAt(currIdx);
  if (!w) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w), styleToBitMask(w->style));
}

void DictionaryWordSelectActivity::prebuildAdvanceTable() {
  // Concatenate every word on the page and OR the style flags. ~2KB transient
  // string; freed on return. Matches FontCacheManager::PrewarmScope's
  // scanText_ allocation pattern.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t pageStyleMask = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const auto& blockWords = block->getWords();
    const auto& blockStyles = block->getWordStyles();
    for (size_t i = 0; i < blockWords.size(); i++) {
      pageText.append(blockWords[i]);
      pageText.push_back(' ');
      if (i < blockStyles.size()) {
        pageStyleMask |= styleToBitMask(blockStyles[i]);
      }
    }
  }
  if (pageStyleMask == 0) pageStyleMask = styleToBitMask(EpdFontFamily::REGULAR);
  // The advance table persists across clearCache() (SdCardFont.h:201) so
  // this only pays the SD cost on the first entry; subsequent ones
  // amortize.
  renderer.ensureSdCardFontReady(SETTINGS.getReaderFontId(), pageText.c_str(), pageStyleMask);
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows, std::string& textPool) {
  words.clear();
  words.reserve(64);
  rows.clear();
  rows.reserve(16);

  // Populate the SD font's advance table once so every getTextAdvanceX call
  // below takes the fast in-RAM path.
  prebuildAdvanceTable();

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    const auto& wordList = block->getWords();
    const auto& xPosList = block->getWordXpos();
    const auto& styleList = block->getWordStyles();
    const auto& focusBoundaryList = block->getWordFocusBoundary();
    const auto& focusSuffixXList = block->getWordFocusSuffixX();

    auto wordIt = wordList.begin();
    auto xIt = xPosList.begin();
    auto styleIt = styleList.begin();

    while (wordIt != wordList.end() && xIt != xPosList.end()) {
      const size_t wordIndex = static_cast<size_t>(wordIt - wordList.begin());
      int16_t screenX = line->xPos + static_cast<int16_t>(*xIt) + marginLeft;
      int16_t screenY = line->yPos + marginTop;
      const std::string& wordText = *wordIt;
      const EpdFontFamily::Style wordStyle = (styleIt != styleList.end()) ? *styleIt : EpdFontFamily::REGULAR;
      const bool hasFocus = wordIndex < focusBoundaryList.size() && wordIndex < focusSuffixXList.size();
      const FocusReading::Annotation focus{hasFocus ? focusBoundaryList[wordIndex] : 0,
                                           hasFocus ? focusSuffixXList[wordIndex] : 0};

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
        const int16_t measuredWidth =
            measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), wordText, wordStyle, hasFocus ? &focus : nullptr);
        // Use the focus-aware advance (measuredWidth) as the base, then add any italic
        // overhang: getTextWidth() tracks bitmap extents (left+width per glyph) while
        // getTextAdvanceX() tracks cursor advance; the difference is the overhang past
        // the advance point. This avoids the old xpos-derivation path
        // (nextXPos - currentXPos - lineGapWidth), which was inflated on justified lines
        // when lineGapWidth fell back to naturalSpaceWidth due to a continuation first pair.
        const int overhang =
            std::max(0, renderer.getTextWidth(SETTINGS.getReaderFontId(), wordText.c_str(), wordStyle) -
                            renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), wordText.c_str(), wordStyle));
        const int16_t wordWidth = static_cast<int16_t>(measuredWidth + overhang);
        WordSelectNavigator::appendWord(words, textPool, wordText.c_str(), wordText.size(), /*lookup=*/nullptr, 0,
                                        screenX, screenY, wordWidth, wordStyle, SETTINGS.getReaderFontId(),
                                        /*isDictFont=*/false, focus.boundary, focus.suffixX);
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
          // Dash-split words are rare (~0-2 per page); per-part measurement
          // is fine here. Soft-hyphen stripping matches the rest of
          // extractWords and matches layout's preprocessor.
          int16_t offsetX =
              prefix.empty() ? 0 : measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), prefix, wordStyle);
          int16_t partWidth =
              static_cast<int16_t>(renderer.getTextWidth(SETTINGS.getReaderFontId(), part.c_str(), wordStyle));
          WordSelectNavigator::appendWord(words, textPool, part.c_str(), part.size(), /*lookup=*/nullptr, 0,
                                          static_cast<int16_t>(screenX + offsetX), screenY, partWidth, wordStyle,
                                          SETTINGS.getReaderFontId(), /*isDictFont=*/false);
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
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, textPool);

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordIndices.empty(); }),
      rows.end());

  // Cross-page hyphenation: update lookup text when the last word on this page
  // ends with a hyphen and its continuation begins the next page.
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().wordIndices.back();
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen > 0 && utf8EndsWithHyphen(lastWord, lastLen) && lastWord[0] != '-') {
      std::string firstPart(lastWord, lastLen);
      utf8RemoveTrailingHyphen(firstPart);
      std::string merged = firstPart + nextPageFirstWord;
      uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
      words[lastWordIdx].lookupOffset = off;
      words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    }
  }
}

void DictionaryWordSelectActivity::loop() {
  const bool shortLookupPressed =
      ReaderUtils::shortPowerButtonActionTriggered(mappedInput, CrossPointSettings::SHORT_PWRBTN::LOOKUP);

  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        replaceActivityKeepingStack(std::make_unique<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(), true, cachePath,
            controller.getRecordHistory(), controller.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller.getFoundStatus())));
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

  if (shortLookupPressed) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

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
  //   - we set it up on the previous frame (Mode::Differential),
  //   - the controller has nothing pending to draw,
  //   - we have a current selection.
  if (diffRepaint_.canDifferential() && !controller.isActive() && currIdx >= 0) {
    prewarmHighlightGlyphs(currIdx);
    auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, diffRepaint_.prevHighlightIdx, currIdx);
    if (dirty.has_value()) {
      // Push full panel — the SDK's windowed-refresh path produces alternating black→white
      // transition failures on consecutive fast partial refreshes, so it's intentionally not
      // wired up here. The savings come from skipping page->render, which dominates the
      // pre-optimization cost; the full push at the end is a hardware floor (~444ms).
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      diffRepaint_.recordDifferentialPush(currIdx);
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
      // Clear the bottom strip the caller reserved (status bar OR auto-turn
      // label). Match the menu→lookup path, which wipes via clearScreen() +
      // page->render(); we skipped both, so clear that one region instead.
      if (reservedBottomHeight_ > 0) {
        int bezelTop, bezelRight, bezelBottom, bezelLeft;
        renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
        const int clearY = renderer.getScreenHeight() - bezelBottom - reservedBottomHeight_;
        const int clearW = renderer.getScreenWidth() - bezelLeft - bezelRight;
        renderer.clearRect(bezelLeft, clearY, clearW, reservedBottomHeight_);
      }

      prewarmHighlightGlyphs(currIdx);

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
      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP_SHORT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      diffRepaint_.primeAfterFullRepaint(currIdx, snapshotPrimed);
      return;
    }
    // Flag was set but conditions weren't met (controller active or no
    // current selection). Fall through to the normal full-repaint path.
  }

  // Full repaint path.
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    diffRepaint_.reset();
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP_SHORT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  diffRepaint_.primeAfterFullRepaint(currIdx, snapshotPrimed);
}
