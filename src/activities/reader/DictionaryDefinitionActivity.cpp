#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "components/UITheme.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;
constexpr float PLAIN_TEXT_LINE_SPACING = 1.5f;
constexpr size_t LAYOUT_HEAP_HEADROOM_BYTES = 8 * 1024;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  const char* plainTextReason = nullptr;
  if (!htmlDefinition) {
    plainTextReason = "dictionary format is not HTML";
  } else if (definition.size() > MAX_STYLED_HTML_BYTES) {
    plainTextReason = "HTML exceeds 16 KiB limit";
  } else if (!layoutHtmlPages()) {
    plainTextReason = "HTML layout failed; see DHTML/EHP error";
  }
  if (plainTextReason) {
    LOG_INF("DICT", "Plain-text definition: %s (bytes=%u)", plainTextReason, static_cast<unsigned>(definition.size()));
    if (htmlDefinition) definition = htmlToPlainText(std::move(definition));
    plainTextLayoutFailed = !wrapText();
  } else {
    LOG_INF("DICT", "Styled definition: %u pages", static_cast<unsigned>(pages.size()));
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int contentLeft =
      std::max(left, orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0);
  const int contentRight =
      std::max(right, orientation == GfxRenderer::Orientation::LandscapeCounterClockwise ? hintGutterWidth : 0);
  const int x = contentLeft + metrics.contentSidePadding;
  const int y = std::max(top, isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight +
                metrics.verticalSpacing;
  const int bottomArea = std::max(bottom, metrics.buttonHintsHeight) + metrics.verticalSpacing;
  return {x, y, renderer.getScreenWidth() - contentLeft - contentRight - 2 * metrics.contentSidePadding,
          renderer.getScreenHeight() - y - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) {
    LOG_ERR("DHTML", "Invalid definition viewport: %dx%d", body.width, body.height);
    return false;
  }
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
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
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

bool DictionaryDefinitionActivity::reserveLines(const size_t capacity) {
  if (capacity <= lineCapacity) return true;
  if (ESP.getMaxAllocHeap() < capacity * sizeof(Line) + LAYOUT_HEAP_HEADROOM_BYTES) {
    LOG_ERR("DICT", "Low heap for %u byte definition line table", static_cast<unsigned>(capacity * sizeof(Line)));
    return false;
  }
  auto buffer = makeUniqueNoThrow<Line[]>(capacity);
  if (!buffer) {
    LOG_ERR("DICT", "OOM: %u byte definition line table", static_cast<unsigned>(capacity * sizeof(Line)));
    return false;
  }
  if (lineCount > 0) std::copy_n(lines.get(), lineCount, buffer.get());
  lines = std::move(buffer);
  lineCapacity = capacity;
  return true;
}

bool DictionaryDefinitionActivity::prepareFont(const int fontId) {
  if (!renderer.isSdCardFont(fontId)) return true;
  const bool hasRtl =
      std::any_of(definition.begin(), definition.end(), [](unsigned char c) { return c >= 0xD6 && c <= 0xDB; });
  if (!hasRtl) {
    renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);
  } else {
    // Font preparation builds shaped RTL text; bound it to the measured span size.
    for (size_t start = 0; start < definition.size();) {
      if (ESP.getMaxAllocHeap() < LAYOUT_HEAP_HEADROOM_BYTES) {
        LOG_ERR("DICT", "Low heap for definition font preparation");
        return false;
      }
      size_t end = std::min(start + MAX_LINE_BYTES, definition.size());
      while (end > start + 1 && end < definition.size() && (definition[end] & 0xC0) == 0x80) end--;
      const char saved = definition[end];
      definition[end] = '\0';
      renderer.ensureSdCardFontReady(fontId, definition.c_str() + start, 0x01 /* REGULAR */);
      definition[end] = saved;
      start = end;
    }
  }
  if (ESP.getMaxAllocHeap() < LAYOUT_HEAP_HEADROOM_BYTES) {
    LOG_ERR("DICT", "Low heap after definition font preparation");
    return false;
  }
  return true;
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
bool DictionaryDefinitionActivity::wrapText() {
  lineCount = 0;
  if (!reserveLines(definition.size() / 32 + 8)) return false;

  const int fontId = SETTINGS.getReaderFontId();
  if (!prepareFont(fontId)) return false;

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId, PLAIN_TEXT_LINE_SPACING);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    if (lineCount == lineCapacity && !reserveLines(lineCapacity * 2)) return false;
    lines[lineCount++] = {lineStart, static_cast<uint16_t>(lineEnd - lineStart)};
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
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
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

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
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
  while (lineCount > 0 && lines[lineCount - 1].len == 0) lineCount--;

  totalPages = std::max(1, (static_cast<int>(lineCount) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
  return true;
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        haptic_feedback::touchAction();
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      haptic_feedback::touchAction();
      currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId, PLAIN_TEXT_LINE_SPACING);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lineCount));
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
  if (plainTextLayoutFailed) {
    GUI.drawPopup(renderer, tr(STR_DICT_LOW_MEMORY));
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  char counter[16] = {};
  if (totalPages > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
  }
  GUI.drawHeader(renderer, Rect{contentX, contentY + metrics.topPadding, contentWidth, metrics.headerHeight},
                 headword.c_str(), counter[0] ? counter : nullptr);

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const BodyArea body = bodyArea();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, body.x, body.y);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, body.x, body.y);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
