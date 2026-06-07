#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <memory>
#include <numeric>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"
#include "util/TextPool.h"

static constexpr char kBullet[] = "- ";

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  wrapText();
  requestUpdate();
  // SD write overlaps the e-ink refresh kicked by requestUpdate() on the render task.
  LookupHistory::addWordIf(cachePath, historyWord, historyStatus, recordHistory);

  // Seed the back-nav chain. The initial word is the newest history entry iff it
  // was just logged (same condition addWordIf applies internally).
  chain_.reset(SETTINGS.getLookupHistoryCapValue());
  const bool initialLogged = recordHistory && !historyWord.empty() && !cachePath.empty();
  chain_.setCurrentHistIndex(initialLogged ? 0 : -1);
}

void DictionaryDefinitionActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

int DictionaryDefinitionActivity::getLineHeight() const {
  return static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                          SETTINGS.getDefinitionLineCompression());
}

// ---------------------------------------------------------------------------
// Layout helpers — shared setup
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapText() {
  isWordSelectMode = false;
  navigator.reset();
  currentPage = 0;  // new definition always starts at page 0

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding + SETTINGS.screenMargin;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
  bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / getLineHeight();
  if (linesPerPage < 1) linesPerPage = 1;

  loadPage(currentPage);
}

// Re-parse the definition and lay out ONLY `page` into layoutLines. The wrap
// produces every line, but collectLineSink keeps only this page's lines (the
// rest are produced then dropped, so peak RAM is one page, not the whole
// definition) and counts all lines to recompute totalPages. Called on entry and
// on every page turn (Stage 2a: re-parse every turn, both directions).
void DictionaryDefinitionActivity::loadPage(int page) {
  layoutLines.clear();
  layoutLines.reserve(static_cast<size_t>(linesPerPage) + 1);
  pagePool_.clear();
  collectTargetPage_ = page;
  collectLineCount_ = 0;

  // Choose rendering path based on dictionary content type
  const DictInfo info = Dictionary::readInfo(foundLocation.folderPath.c_str());
  if (info.valid && info.sametypesequence[0] == 'h') {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = DictLayout::paginate(collectLineCount_, linesPerPage);
}

void DictionaryDefinitionActivity::collectLineSink(void* ctx, DictLayout::LayoutLine&& line) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int idx = self->collectLineCount_++;
  const int start = self->collectTargetPage_ * self->linesPerPage;
  if (idx < start || idx >= start + self->linesPerPage) return;  // not on this page — discard

  // Pool the kept line's text: each (already same-style-merged) segment becomes
  // one null-terminated pool entry referenced by {offset, len}.
  PooledLine pooled;
  pooled.indentLevel = line.indentLevel;
  pooled.isListItem = line.isListItem;
  pooled.segments.reserve(line.segments.size());
  for (const auto& seg : line.segments) {
    PooledSegment ps;
    ps.offset = TextPool::append(self->pagePool_, seg.text.c_str(), seg.text.size());
    ps.len = static_cast<uint16_t>(seg.text.size());
    ps.style = seg.style;
    ps.isIpa = seg.isIpa;
    pooled.segments.push_back(ps);
  }
  self->layoutLines.push_back(std::move(pooled));
}

// ---------------------------------------------------------------------------
// Shared helper: measure text width accounting for mixed IPA/non-IPA runs
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text,
                                                EpdFontFamily::Style style) {
  ipaRuns.clear();
  splitIpaRuns(text, ipaRuns);
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum +
           renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId(), run.text.c_str(), style);
  });
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isIpa) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
  return self->renderer.getTextWidth(fontId, text, style);
}

void DictionaryDefinitionActivity::wrapHtml() {
  const int maxWidth = renderer.getScreenWidth() - leftPadding - rightPadding;
  // Indent step: 3 spaces worth of pixels at regular weight.
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");
  const int bulletWidth = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);

  // Fully streamed: the renderer delivers spans one at a time to the Wrapper, the
  // Wrapper emits completed lines to the page collector, and the collector keeps
  // only the current page. Neither the whole-definition span/textBuf (renderer)
  // nor all pages of lines (here) is ever materialized.
  DictLayout::Measurer measure{this, &DictionaryDefinitionActivity::measureWidthAdapter};
  DictLayout::LineSink lineSink{this, &DictionaryDefinitionActivity::collectLineSink};
  DictLayout::Wrapper wrapper(DictLayout::WrapMetrics{maxWidth, indentStep, bulletWidth}, measure, lineSink);

  // Renderer is a reused activity member (3.1-A): renderFromFileStreaming resets
  // it each call (XML_ParserReset, not free+create), so no per-turn object/parser
  // churn. Streaming means it never materializes the whole-definition buffers.
  const std::string dictPath = foundLocation.folderPath + ".dict";
  const DictHtmlRenderer::SpanSink spanSink{&wrapper, &DictionaryDefinitionActivity::feedSpanToWrapper};
  htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), foundLocation.offset, foundLocation.size, spanSink);
  wrapper.finish();
  // Only the kept page's span text was ever copied into layoutLines.
}

void DictionaryDefinitionActivity::feedSpanToWrapper(void* ctx, const StyledSpan& span) {
  static_cast<DictLayout::Wrapper*>(ctx)->onSpan(span);
}

// ---------------------------------------------------------------------------
// Plain text path: word-wrap into single-segment REGULAR lines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapPlain() {
  std::vector<IpaTextSpan> ipaRuns;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  const int spaceWidth = renderer.getSpaceWidth(SETTINGS.getDefinitionFontId(), EpdFontFamily::REGULAR);

  std::string currentWord;
  std::string currentLineText;
  int currentLineWidth = 0;

  DictLayout::LineSink sink{this, &DictionaryDefinitionActivity::collectLineSink};
  auto flushLine = [&]() {
    if (currentLineText.empty()) return;
    DictLayout::LayoutLine line;
    ipaRuns.clear();
    splitIpaRuns(currentLineText.c_str(), ipaRuns);
    for (const auto& run : ipaRuns) {
      line.segments.push_back({run.text, EpdFontFamily::REGULAR, run.isIpa});
    }
    sink(std::move(line));
    currentLineText.clear();
    currentLineWidth = 0;
  };

  auto tryAppendWord = [&]() {
    if (currentWord.empty()) return;
    const int wordWidth = getMixedWidth(ipaRuns, currentWord.c_str(), EpdFontFamily::REGULAR);
    if (currentLineText.empty()) {
      currentLineText = currentWord;
      currentLineWidth = wordWidth;
    } else {
      const int testWidth = currentLineWidth + spaceWidth + wordWidth;
      if (testWidth <= maxWidth) {
        currentLineText += ' ';
        currentLineText += currentWord;
        currentLineWidth = testWidth;
      } else {
        flushLine();
        currentLineText = currentWord;
        currentLineWidth = wordWidth;
      }
    }
    currentWord.clear();
  };

  // Stream from .dict file — the full definition is never held in RAM.
  const std::string dictPath = foundLocation.folderPath + ".dict";
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) return;
  dictFile.seekSet(foundLocation.offset);

  uint32_t remaining = foundLocation.size;
  char chunk[512];

  while (remaining > 0) {
    uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
    int n = dictFile.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
    if (n <= 0) break;
    remaining -= static_cast<uint32_t>(n);

    for (int ci = 0; ci < n; ci++) {
      char c = chunk[ci];
      if (c == '\n') {
        tryAppendWord();
        flushLine();
      } else if (c == ' ') {
        tryAppendWord();
      } else {
        currentWord += c;
      }
    }
  }

  tryAppendWord();
  flushLine();
  dictFile.close();
}

// ---------------------------------------------------------------------------
// Word-select: extract words from the currently visible page
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::extractWordsFromLayout() {
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");

  std::vector<WordSelectNavigator::WordInfo> words;
  words.reserve(64);
  std::vector<WordSelectNavigator::Row> rows;
  rows.reserve(16);
  std::string textPool;
  textPool.reserve(512);

  const int lineHeight = getLineHeight();  // cached for loop
  for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
    const PooledLine& line = layoutLines[i];
    const int16_t lineY = static_cast<int16_t>(bodyStartY + i * lineHeight);
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      x += renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);
    }

    for (const auto& seg : line.segments) {
      const int segFontId = seg.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
      const int spaceWidth = renderer.getSpaceWidth(segFontId, seg.style);
      const char* p = pagePool_.data() + seg.offset;
      while (*p) {
        while (*p == ' ') {
          x += spaceWidth;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        const size_t tokLen = static_cast<size_t>(p - tokStart);
        std::string tok(tokStart, tokLen);

        const int tokVisualWidth = renderer.getTextWidth(segFontId, tok.c_str(), seg.style);
        const int tokAdvanceX = renderer.getTextAdvanceX(segFontId, tok.c_str(), seg.style);
        std::string cleaned = Dictionary::cleanWord(tok);
        if (!cleaned.empty()) {
          uint16_t tokOff = WordSelectNavigator::poolAppend(textPool, tok.c_str(), tok.size());
          uint16_t cleanedOff = WordSelectNavigator::poolAppend(textPool, cleaned.c_str(), cleaned.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = tokOff;
          wi.textLen = static_cast<uint16_t>(tok.size());
          wi.lookupOffset = cleanedOff;
          wi.lookupLen = static_cast<uint16_t>(cleaned.size());
          wi.screenX = static_cast<int16_t>(x);
          wi.screenY = lineY;
          wi.width = static_cast<int16_t>(tokVisualWidth);
          wi.style = seg.style;
          wi.isIpa = seg.isIpa;
          wi.fontId = segFontId;
          words.push_back(wi);
        }
        x += tokAdvanceX;
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
  navigator.load(std::move(words), std::move(rows), std::move(textPool));
}

// ---------------------------------------------------------------------------
// Input loop
// ---------------------------------------------------------------------------

bool DictionaryDefinitionActivity::handleLongPressExitAll(bool enabled) {
  if (enabled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    setResult(ActivityResult{});
    finish();
    return true;
  }
  return false;
}

void DictionaryDefinitionActivity::loop() {
  // --- Controller active (LookingUp / AltFormPrompt / NotFound) ---
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        const bool wasBackNav = chainBackNavInProgress;
        const bool willLog = !wasBackNav && controller.getRecordHistory();
        if (!wasBackNav) {
          // Forward: push a back-entry for the word being left (current headword,
          // on currentPage), referencing its history position.
          chain_.onForward(static_cast<uint16_t>(currentPage), willLog);
        }
        chainBackNavInProgress = false;
        headword = controller.getFoundWord();
        foundLocation = controller.getFoundLocation();
        wrapText();  // resets currentPage to 0 and loads page 0
        if (wasBackNav) {
          // Re-derive the now-current word's history position and restore its page.
          chain_.setCurrentHistIndex(pendingBack_.histIndex);
          currentPage = (pendingBack_.page < totalPages) ? pendingBack_.page : (totalPages - 1);
          if (currentPage < 0) currentPage = 0;
          if (currentPage > 0) loadPage(currentPage);
        }
        isWordSelectMode = false;
        requestUpdate();
        // Chain-forward records; chain-back-nav does not.
        LookupHistory::addWordIf(cachePath, controller.getLookupWord(),
                                 DictionaryLookupController::toHistStatus(controller.getFoundStatus()), willLog);
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // --- Word-select mode ---
  if (isWordSelectMode) {
    if (navigator.handleNavigation(mappedInput, renderer)) {
      requestUpdate();
    }

    if (controller.handleMultiSelect(navigator)) return;

    if (!navigator.isMultiSelecting()) {
      if (controller.handleConfirmLookup(navigator)) return;

      if (handleLongPressExitAll(true)) return;

      // Short press Back: exit word-select mode.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
      }
    }
    return;
  }

  // --- View mode ---
  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevPage && currentPage > 0) {
    currentPage--;
    loadPage(currentPage);
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    loadPage(currentPage);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (showLookupButton) {
      extractWordsFromLayout();
      if (!navigator.isEmpty()) {
        isWordSelectMode = true;
        requestUpdate();
      }
    } else {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (handleLongPressExitAll(showLookupButton)) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      (!showLookupButton || mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS)) {
    if (!cachePath.empty() && !chain_.empty()) {
      pendingBack_ = chain_.pop();
      // Resolve the prior headword from the persisted history by distance-from-newest.
      const auto hist = LookupHistory::load(cachePath);  // newest-first
      if (pendingBack_.histIndex < hist.size()) {
        chainBackNavInProgress = true;
        controller.startLookup(hist[pendingBack_.histIndex].word, false);
        return;
      }
      // Unresolvable (should not happen under the depth cap) — fall through to exit.
    }
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::render(RenderLock&&) {
  // Differential fast path: only when we're already in word-select mode AND
  // we set it up on the previous frame AND the controller has nothing pending.
  if (isWordSelectMode && nextRenderMode_ == RenderMode::Differential && !controller.isActive()) {
    const int currIdx = navigator.getCurrentFlatIndex();
    if (currIdx >= 0) {
      const int lineHeight = getLineHeight();
      auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx);
      if (dirty.has_value()) {
        // Full panel push — matches DictionaryWordSelectActivity. Windowed refresh is not
        // wired up because the SDK's experimental path produces alternating black→white
        // failures on consecutive partial refreshes. Savings come from skipping page->render.
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        prevHighlightIdx_ = currIdx;
        return;
      }
      // fall through to full repaint path
    }
  }

  // Full repaint path.
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  const auto metrics = UITheme::getInstance().getMetrics();
  const int indentStep = renderer.getTextWidth(SETTINGS.getDefinitionFontId(), "   ");

  // Header
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());

  // Body: draw layout lines for the current page (BW pass). layoutLines holds
  // only the current page (Stage 2a streaming), so it is indexed from 0.
  const int lineHeight = getLineHeight();  // cached for loop + renderHighlight
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
      const PooledLine& line = layoutLines[i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(SETTINGS.getDefinitionFontId(), x, y, kBullet);
        x += renderer.getTextWidth(SETTINGS.getDefinitionFontId(), kBullet);
      }

      for (const auto& seg : line.segments) {
        const int segFontId = seg.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId();
        const char* segText = pagePool_.data() + seg.offset;
        renderer.drawText(segFontId, x, y, segText, true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, segText, seg.style);
          const int underlineY = y + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, segText, seg.style);
      }
    }
  };
  renderBody();

  // Word-select mode: overlay highlighted word(s) and prime snapshot for next frame.
  // The -1 prevWordIdx literal is load-bearing: renderHighlightDifferential uses
  // prevWordIdx < 0 as the signal "framebuffer was just redrawn from scratch,
  // discard any stale snapshot rather than restoring it on top of fresh pixels."
  // This is the only path that disturbs the framebuffer outside the differential
  // cycle, so it's also the only call site that must pass -1.
  if (isWordSelectMode) {
    const int currIdx = navigator.getCurrentFlatIndex();
    bool snapshotPrimed = false;
    if (currIdx >= 0) {
      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
      snapshotPrimed = setup.has_value();
    }
    if (!snapshotPrimed) {
      navigator.renderHighlight(renderer, lineHeight);
    }

    // Empty button hints in word-select mode (same convention as EPUB word-select)
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    prevHighlightIdx_ = currIdx;
    nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
    return;
  }

  // View mode: differential state is irrelevant — reset so that the next entry
  // into word-select starts cleanly with a full repaint.
  nextRenderMode_ = RenderMode::FullPage;
  prevHighlightIdx_ = -1;

  // Pagination indicator and button hints
  if (totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing, pageInfo);
  }

  const char* btn2 = showLookupButton ? tr(STR_LOOKUP_SHORT) : "";
  const char* btn3 = totalPages > 1 ? tr(STR_DIR_UP) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Anti-aliasing pass: overlay grayscale body text on top of the BW display
  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, renderBody);
  }
}
