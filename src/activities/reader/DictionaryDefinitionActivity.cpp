#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HeapFit.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/ActivityResult.h"
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas are roughly the text plus ~7 bytes per word, plus per-line
// objects), so they roughly double the string's footprint while this activity is
// stacked over the reader and the word-select overlay. Bigger definitions take
// the span-based plain-text path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the C-string
  // font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    htmlToPlainTextInPlace(definition);
    if (!wrapText()) {
      // Say so rather than show part of the definition as if it were all of
      // it. Nothing draws either buffer now, so both go back to the heap.
      layoutFailed = true;
      std::vector<Line>().swap(lines);
      definition.clear();
      definition.shrink_to_fit();
    }
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  // A definition can pull in a whole SD font's glyphs for a script the book never uses, and
  // those caches would otherwise stay resident behind the reader for the rest of the session.
  // They rebuild on demand, so handing the heap back on the way out costs a reload at worst
  // and buys back tens of KB the reader wants for its next page build.
  // Ported from crosspoint-reader PR #3317 (Uri Tauber <uritaube@gmail.com>). Theirs calls
  // FontCacheManager::releaseSdFontCaches(); ours is named clearCache() and does the same two
  // things -- drop the decompressor cache, then clear every SdCardFont.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->clearCache();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int topArea = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.verticalSpacing;
  return {contentRect.x + SIDE_PADDING, topArea, contentRect.width - 2 * SIDE_PADDING,
          contentRect.height - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser into
// reader-identical Pages. Frees `definition` on success (the page arenas own the
// text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, DICTIONARY_FONT_ID, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextWidth(fontId, buf);
}

// Grows the line table to `capacity` only when the heap has the block for it.
// A definition runs to 64 KB, and its table to a quarter of that, so a failed
// growth -- an abort with -fno-exceptions -- is a real outcome on a heap the
// definition itself has just drawn down.
//
// Ported from crosspoint-reader PR #3988 (Uri Tauber), which checks each growth
// of its line table the same way; it is kept a std::vector here, guarded by the
// shared heapHasBlockFor().
bool DictionaryDefinitionActivity::reserveLines(const size_t capacity) {
  if (capacity <= lines.capacity()) return true;
  if (!heapHasBlockFor(capacity * sizeof(Line))) {
    LOG_ERR("DICT", "No %u-byte block for the definition's line table (%u lines)",
            static_cast<unsigned>(capacity * sizeof(Line)), static_cast<unsigned>(capacity));
    return false;
  }
  lines.reserve(capacity);
  return true;
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries were
// normalized to newlines in onEnter); '\r' is dropped by treating it as a space
// at a token edge.
//
// False when the line table could not grow.
bool DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  // The size-based estimate (about one line per 32 bytes) first: one block and
  // no regrowth for ordinary prose. When even that block is missing, a small
  // start still serves a definition that turns out to need fewer lines, and
  // growth below is checked the same way.
  constexpr size_t MIN_LINE_CAPACITY = 64;
  if (!reserveLines(definition.size() / 32 + 8) && !reserveLines(MIN_LINE_CAPACITY)) return false;

  const int fontId = DICTIONARY_FONT_ID;
  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const auto n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    // Half again rather than double: the smaller block is the likelier find on
    // the fragmented heap this guards against.
    if (lines.size() == lines.capacity() && !reserveLines(lines.capacity() + lines.capacity() / 2)) return false;
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
    return true;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      if (!flushLine(i + 1)) return false;
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A natural
    // stop lands on whitespace or the terminating NUL, never on a continuation
    // byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      if (!flushLine(tokenStart)) return false;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line (any
    // previous content was flushed above): split it at the widest fitting UTF-8
    // boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      if (!flushLine(rest)) return false;
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart && !flushLine(n)) return false;

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
  return true;
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Confirm is the only button page navigation does not already own (next and
  // previous each take two), so it cycles forward through the installed
  // dictionaries. Forward only: with two installed that is both directions
  // anyway, and with more it still reaches every one.
  //
  // The switch itself belongs to the overlay above, which owns the open
  // Dictionary and knows the word -- this just asks, and closes.
  if (canSwitchDictionary() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(DictionarySwitchResult{1});
    finish();
    return;
  }

  const auto nextPage = [this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  };
  const auto prevPage = [this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  };

  // A definition long enough to page turns by tapping the two halves of its text, the way a book
  // page does. Bounded to the body rectangle rather than the reader's full-height zones: this
  // screen draws a hint strip, and full-height zones would claim the taps meant for it.
  //
  // Only while there is somewhere to go, so a one-page entry leaves every tap to the strip.
  int tapX = 0;
  int tapY = 0;
  if (totalPages > 1 && mappedInput.hasTouch() && mappedInput.wasScreenTapped(tapX, tapY)) {
    const BodyArea body = bodyArea();
    switch (TapZones::halfOfBand(tapX, tapY, body.x, body.y, body.width, body.height)) {
      case TapZones::Half::Previous:
        prevPage();
        return;
      case TapZones::Half::Next:
        nextPage();
        return;
      case TapZones::Half::None:
        break;
    }
  }

  buttonNavigator.onNext(nextPage);
  buttonNavigator.onPrevious(prevPage);
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once for
// the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Header: matched headword left, page counter right.
  const int headerY = metrics.topPadding;
  GUI.drawHeader(renderer, Rect{contentRect.x, headerY, contentRect.width, metrics.headerHeight}, headword.c_str());

  // Right of the header: which dictionary this came from, and which page of it.
  // The name only appears when there is more than one installed -- otherwise it
  // is noise, since it can neither change nor be chosen between.
  char status[64];
  if (canSwitchDictionary() && totalPages > 1) {
    snprintf(status, sizeof(status), "%s  %d/%d", dictionaryName.c_str(), currentPage + 1, totalPages);
  } else if (canSwitchDictionary()) {
    snprintf(status, sizeof(status), "%s", dictionaryName.c_str());
  } else if (totalPages > 1) {
    snprintf(status, sizeof(status), "%d/%d", currentPage + 1, totalPages);
  } else {
    status[0] = '\0';
  }
  if (status[0] != '\0') {
    // A long dictionary name would run under the headword; drop leading
    // characters until it fits rather than overprinting.
    const int available = contentRect.width / 2;
    const char* shown = status;
    while (*shown != '\0' && renderer.getTextWidth(UI_10_FONT_ID, shown) > available) shown++;
    const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, shown);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + contentRect.width - SIDE_PADDING - statusWidth,
                      headerY + metrics.headerHeight / 2, shown);
  }

  if (layoutFailed) {
    // No body to page through: the headword stays above the message, and Back
    // (or Confirm, to try another dictionary) still answers below it.
    GUI.drawPopup(renderer, tr(STR_DICT_LOW_MEMORY), /*overlayDisplayedFrame=*/false, PopupShip::Caller);
  } else {
    // Body: two-pass draw inside a prewarm scope (the same pattern the reader's
    // page render uses) so SD-card font glyphs load from the card in one batch
    // instead of one on-demand read per character on every page turn.
    const int fontId = DICTIONARY_FONT_ID;
    // The same rectangle the tap-to-page split is resolved against, so what is drawn and what
    // answers a finger cannot drift apart.
    const BodyArea body = bodyArea();
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    drawBody(fontId, body.x, body.y);  // scan pass: records codepoints only
    scope.endScanAndPrewarm();
    drawBody(fontId, body.x, body.y);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canSwitchDictionary() ? tr(STR_DICTIONARY) : "",
                                            (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
