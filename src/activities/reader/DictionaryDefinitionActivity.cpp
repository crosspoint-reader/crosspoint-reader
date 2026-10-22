#include "DictionaryDefinitionActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "HapticFeedback.h"
#include "ReaderUtils.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;
constexpr float PLAIN_TEXT_LINE_SPACING = 1.5f;
constexpr size_t HEAP_HEADROOM = 8 * 1024;
constexpr unsigned long MESSAGE_DURATION_MS = 1500;
constexpr size_t SELECTION_PAGE_WINDOW = 3;
// ASCII tokens need separators; Han/Kana tokens take at least three bytes.
constexpr size_t MAX_PLAIN_WORDS = (MAX_LINE_BYTES + 1) / 2;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

// Heap-owned only while preparing plain selection pages; keep the loop task's stack small.
struct DictionaryDefinitionActivity::PlainSelectionLine {
  std::string text;
  std::vector<std::string> words;
  std::vector<int16_t> positions;
  std::vector<EpdFontFamily::Style> styles;
  std::vector<TextBlock::SourceRange> ranges;
  std::vector<uint16_t> visualOrder;
  std::vector<std::string> visualWords;
  std::vector<TextBlock::SourceRange> visualRanges;
  BlockStyle style;

  void layout(const DictionaryDefinitionActivity& owner, const int fontId, const uint32_t offset) {
    const char* bytes = text.c_str();
    const size_t length = text.size();
    words.clear();
    positions.clear();
    styles.clear();
    ranges.clear();
    size_t i = 0;
    while (i < length) {
      // The headword label marker is a control byte, not text: it must not
      // end up inside a selected word.
      if (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == DICTIONARY_PLAIN_HEADWORD_MARKER) {
        ++i;
        continue;
      }
      const size_t start = i;
      auto* ptr = reinterpret_cast<const unsigned char*>(bytes + i);
      uint32_t previous = utf8NextCodepoint(&ptr);
      i = ptr - reinterpret_cast<const unsigned char*>(bytes);
      while (i < length && bytes[i] != ' ' && bytes[i] != '\t' && bytes[i] != '\r' &&
             bytes[i] != DICTIONARY_PLAIN_HEADWORD_MARKER) {
        ptr = reinterpret_cast<const unsigned char*>(bytes + i);
        const uint32_t cp = utf8NextCodepoint(&ptr);
        if (!utf8IsHangul(previous) && !utf8IsHangul(cp) && (utf8IsCjkBreakable(previous) || utf8IsCjkBreakable(cp)) &&
            !utf8IsCombiningMark(cp))
          break;
        previous = cp;
        i = ptr - reinterpret_cast<const unsigned char*>(bytes);
      }
      words.emplace_back(bytes + start, i - start);
      styles.push_back(EpdFontFamily::REGULAR);
      ranges.push_back({static_cast<uint32_t>(offset + start), static_cast<uint32_t>(offset + i)});
    }
    style.isRtl = BidiUtils::detectParagraphLevel(bytes) == 1;
    const bool reordered = BidiUtils::computeVisualWordOrder(words, style.isRtl, visualOrder);
    int x = 0;
    for (size_t n = 0; n < words.size(); ++n) {
      const size_t index = reordered ? visualOrder[n] : n;
      if (n > 0) {
        const size_t previous = reordered ? visualOrder[n - 1] : n - 1;
        const size_t left = std::min(previous, index);
        const size_t right = std::max(previous, index);
        x += right == left + 1 ? owner.measureSpan(fontId, text.c_str() + ranges[left].end - offset,
                                                   ranges[right].start - ranges[left].end)
                               : owner.renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
      }
      positions.push_back(x);
      x += owner.renderer.getTextAdvanceX(fontId, words[index].c_str(), EpdFontFamily::REGULAR);
    }
    if (reordered) {
      visualWords.clear();
      visualRanges.clear();
      for (const auto index : visualOrder) {
        visualWords.push_back(std::move(words[index]));
        visualRanges.push_back(ranges[index]);
      }
      words.swap(visualWords);
      ranges.swap(visualRanges);
    }
  }

  std::unique_ptr<TextBlock> pack() const {
    return makeUniqueNoThrow<TextBlock>(words, positions, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{},
                                        style, std::vector<std::string>{}, std::vector<TextBlock::LinkSpan>{}, ranges);
  }
};

bool DictionaryDefinitionActivity::enableLookup(Dictionary& dictionary, const DictLocation location) {
  lookup = makeUniqueNoThrow<LookupState>(dictionary, location);
  if (!lookup) LOG_ERR("DICT", "OOM: lookup history (%u bytes)", static_cast<unsigned>(sizeof(LookupState)));
  return lookup != nullptr;
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  initialLayoutFailed = !prepareDefinition(content);
  if (initialLayoutFailed) {
    content.lines.clear();
    showMessage(StrId::STR_DICT_LOW_MEMORY);
  }
  requestUpdate();
}

bool DictionaryDefinitionActivity::prepareDefinition(Definition& next, const bool forcePlain) {
  std::replace(next.text.begin(), next.text.end(), '\0', '\n');
  if (htmlDefinition && !forcePlain && next.text.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages(next)) return true;
  if (htmlDefinition) {
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.largestBlockBytes < next.text.size() + 1 || heap.freeBytes < next.text.size() + HEAP_HEADROOM) {
      LOG_ERR("DICT", "Low heap for plain definition (%u bytes)", static_cast<unsigned>(next.text.size() + 1));
      return false;
    }
    next.text = htmlToPlainText(next.text, DICTIONARY_PLAIN_HEADWORD_MARKER);
  }
  return wrapText(next);
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
bool DictionaryDefinitionActivity::layoutHtmlPages(Definition& next) {
  auto& definition = next.text;
  auto& pages = next.pages;
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
  next.totalPages = static_cast<int>(pages.size());
  next.currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len,
                                              const EpdFontFamily::Style family) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, family);
}

bool DictionaryDefinitionActivity::reserveLines(Definition& next, const size_t count) {
  if (next.lines.capacity() >= count) return true;
  const size_t bytes = count * sizeof(Line);
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.largestBlockBytes < bytes || heap.freeBytes < bytes + HEAP_HEADROOM) {
    LOG_ERR("DICT", "Low heap for wrapped lines (%u bytes)", static_cast<unsigned>(bytes));
    return false;
  }
  next.lines.reserve(count);
  return true;
}

bool DictionaryDefinitionActivity::wrapText(Definition& next) {
  auto& definition = next.text;
  auto& lines = next.lines;
  lines.clear();
  if (!reserveLines(next, definition.size() / 32 + 8)) return false;

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x03 /* REGULAR + BOLD */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId, PLAIN_TEXT_LINE_SPACING);
  next.linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;
  bool lineBold = false;

  const auto flushLine = [&](uint32_t nextStart) {
    if (lines.size() == lines.capacity() && !reserveLines(next, lines.capacity() * 2)) return false;
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart), lineBold});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
    lineBold = false;
    return true;
  };

  uint32_t i = 0;
  while (i < n) {
    // A marker at the start of a line makes that whole line bold. It stands
    // for a <h2> in the HTML path, and for a synonym's headword label in the
    // plain-text path.
    if (lineEnd == lineStart && i == lineStart && text[i] == DICTIONARY_PLAIN_HEADWORD_MARKER) {
      lineStart = ++i;
      lineEnd = lineStart;
      lineBold = true;
      continue;
    }
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
    const int tokenWidth =
        measureSpan(fontId, text + tokenStart, tokenLen, lineBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

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
          if (measureSpan(fontId, text + lineStart, f, lineBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR) >
              maxWidth)
            break;
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
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart,
                              lineBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  }
  if (lineEnd > lineStart && !flushLine(n)) return false;

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  next.totalPages = std::max(1, (static_cast<int>(lines.size()) + next.linesPerPage - 1) / next.linesPerPage);
  next.currentPage = 0;
  return true;
}

void DictionaryDefinitionActivity::showMessage(const StrId nextMessage) {
  message = nextMessage;
  messageTime = millis();
  messageVisible = true;
  requestUpdate();
}

bool DictionaryDefinitionActivity::restoreCurrentDefinition(const bool styled) {
  const int page = content.currentPage;
  auto result = Dictionary::LookupResult::Found;
  if (!lookup->dictionary.readDefinition(lookup->location, content.text, &result) ||
      !prepareDefinition(content, !styled) || styled != !content.pages.empty() || page >= content.totalPages) {
    LOG_ERR("DICT", "Cannot recover current definition after failed lookup");
    finish();
    return false;
  }
  content.currentPage = page;
  return true;
}

void DictionaryDefinitionActivity::navigateDefinition(const bool back) {
  {
    RenderLock lock;
    showMessage(StrId::STR_DICT_LOOKING_UP);
  }
  requestUpdateAndWait();

  RenderLock lock;
  messageVisible = false;
  const bool styled = !content.pages.empty();
  // Keep only navigation metadata while inflating and laying out another entry.
  content.releasePayload();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  const auto heap = HalMemory::getInternalHeap();
  LOG_DBG("DICT", "Navigation after release: free=%u largest=%u", static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
  Definition next;
  DictLocation location;
  auto result = Dictionary::LookupResult::Found;
  const bool found =
      back ? lookup->dictionary.readDefinition(lookup->history[lookup->depth - 1].location, next.text, &result)
           : lookup->dictionary.lookup(lookup->pendingWord.c_str(), next.text, next.headword, &result, &location);
  lookup->pendingWord.clear();
  const auto recover = [this, back, styled, &next](const StrId error) {
    next.releasePayload();
    std::string{}.swap(next.headword);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    if (back) {
      LOG_ERR("DICT", "Cannot restore previous definition; leaving lookup");
      finish();
    } else if (restoreCurrentDefinition(styled)) {
      showMessage(error);
    }
  };
  if (!found) {
    switch (result) {
      case Dictionary::LookupResult::Found:
      case Dictionary::LookupResult::ReadError:
        recover(StrId::STR_DICT_READ_FAILED);
        break;
      case Dictionary::LookupResult::NotFound:
        recover(StrId::STR_DICT_NOT_FOUND);
        break;
      case Dictionary::LookupResult::LowMemory:
        recover(StrId::STR_DICT_LOW_MEMORY);
        break;
      case Dictionary::LookupResult::Decompress:
        recover(StrId::STR_DICT_DECOMPRESS_ERROR);
        break;
    }
    return;
  }
  const bool forcePlain = back && !lookup->history[lookup->depth - 1].styled;
  if (!prepareDefinition(next, forcePlain)) {
    recover(StrId::STR_DICT_LOW_MEMORY);
    return;
  }
  if (back) {
    auto& previous = lookup->history[lookup->depth - 1];
    if (previous.styled != !next.pages.empty() || previous.page >= next.totalPages) {
      LOG_ERR("DICT", "Cannot restore definition layout");
      recover(StrId::STR_DICT_RESTORE_FAILED);
      return;
    }
    next.headword = std::move(previous.headword);
    next.currentPage = previous.page;
    next.selectionPage = previous.selectionPage;
    next.selection = previous.selection;
    lookup->location = previous.location;
    --lookup->depth;
  } else {
    auto& previous = lookup->history[lookup->depth++];
    previous.headword = std::move(content.headword);
    previous.location = lookup->location;
    previous.page = content.currentPage;
    previous.selectionPage = content.selectionPage;
    previous.selection = content.selection;
    previous.styled = styled;
    lookup->location = location;
  }
  content = std::move(next);
  requestUpdate();
}

bool DictionaryDefinitionActivity::buildPlainSelectionPages() {
  // Only the selection window gets packed word arenas; the definition keeps its cheap line spans.
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId, PLAIN_TEXT_LINE_SPACING);
  const int pageCount = std::min<int>(SELECTION_PAGE_WINDOW, content.totalPages - content.selectionPage);
  const size_t scratchBytes =
      sizeof(PlainSelectionLine) + MAX_LINE_BYTES + 1 +
      MAX_PLAIN_WORDS * (2 * sizeof(std::string) + sizeof(int16_t) + sizeof(EpdFontFamily::Style) +
                         2 * sizeof(TextBlock::SourceRange) + sizeof(uint16_t) + 4);
  auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < scratchBytes + HEAP_HEADROOM || heap.largestBlockBytes < scratchBytes) return false;
  auto scratch = makeUniqueNoThrow<PlainSelectionLine>();
  if (!scratch) {
    LOG_ERR("DICT", "OOM: selection scratch (%u bytes)", static_cast<unsigned>(sizeof(PlainSelectionLine)));
    return false;
  }
  scratch->text.reserve(MAX_LINE_BYTES);
  scratch->words.reserve(MAX_PLAIN_WORDS);
  scratch->positions.reserve(MAX_PLAIN_WORDS);
  scratch->styles.reserve(MAX_PLAIN_WORDS);
  scratch->ranges.reserve(MAX_PLAIN_WORDS);
  scratch->visualOrder.reserve(MAX_PLAIN_WORDS);
  scratch->visualWords.reserve(MAX_PLAIN_WORDS);
  scratch->visualRanges.reserve(MAX_PLAIN_WORDS);
  lookup->selectionPages.reserve(SELECTION_PAGE_WINDOW);
  for (int p = 0; p < pageCount; ++p) {
    auto page = makeUniqueNoThrow<Page>();
    if (!page) {
      LOG_ERR("DICT", "OOM: selection page (%u bytes)", static_cast<unsigned>(sizeof(Page)));
      return false;
    }
    const int firstLine = (content.selectionPage + p) * content.linesPerPage;
    const int lastLine = std::min(firstLine + content.linesPerPage, static_cast<int>(content.lines.size()));
    heap = HalMemory::getDefaultHeap();
    const size_t pageBytes = (lastLine - firstLine) * (sizeof(PageLine) + sizeof(std::unique_ptr<PageElement>));
    if (heap.freeBytes < pageBytes + HEAP_HEADROOM || heap.largestBlockBytes < pageBytes) return false;
    page->elements.reserve(lastLine - firstLine);
    for (int row = firstLine; row < lastLine; ++row) {
      const auto& line = content.lines[row];
      if (!line.len) continue;
      const size_t length =
          utf8SafeTruncateBuffer(content.text.c_str() + line.start, std::min<size_t>(line.len, MAX_LINE_BYTES));
      scratch->text.assign(content.text.c_str() + line.start, length);
      heap = HalMemory::getDefaultHeap();
      const size_t lineBytes = length * (sizeof(TextBlock::SourceRange) + 12) + sizeof(TextBlock) + 1024;
      if (heap.freeBytes < lineBytes + HEAP_HEADROOM || heap.largestBlockBytes < lineBytes) return false;
      scratch->layout(*this, fontId, line.start);
      auto block = scratch->pack();
      if (!block || !block->valid()) {
        LOG_ERR("DICT", "OOM: selection line (%u byte budget)", static_cast<unsigned>(lineBytes));
        return false;
      }
      auto element = makeUniqueNoThrow<PageLine>(std::move(block), 0, (row - firstLine) * lineHeight);
      if (!element) {
        LOG_ERR("DICT", "OOM: selection element (%u bytes)", static_cast<unsigned>(sizeof(PageLine)));
        return false;
      }
      page->elements.push_back(std::move(element));
    }
    lookup->selectionPages.push_back(std::move(page));
  }
  return true;
}

void DictionaryDefinitionActivity::startSelection(const int x, const int y) {
  RenderLock lock;
  if (!content.selection.valid || x >= 0) {
    content.selectionPage = content.currentPage;
    content.selection.valid = false;
  }
  std::span<const std::unique_ptr<Page>> pages;
  if (content.pages.empty()) {
    if (!buildPlainSelectionPages()) {
      LOG_ERR("DICT", "Cannot allocate plain definition selection");
      lookup->selectionPages.clear();
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      return;
    }
    pages = lookup->selectionPages;
  } else {
    pages = std::span(content.pages)
                .subspan(content.selectionPage,
                         std::min<size_t>(SELECTION_PAGE_WINDOW, content.pages.size() - content.selectionPage));
  }
  const auto body = bodyArea();
  auto selection =
      makeUniqueNoThrow<ClipSelectionActivity>(renderer, mappedInput, pages, body.x, body.y, content.selection, x, y);
  if (!selection) {
    LOG_ERR("DICT", "OOM: definition selection (%u bytes)", static_cast<unsigned>(sizeof(ClipSelectionActivity)));
    lookup->selectionPages.clear();
    showMessage(StrId::STR_DICT_LOW_MEMORY);
    return;
  }
  startActivityForResult(std::move(selection), [this](const ActivityResult& result) {
    RenderLock lock;
    lookup->selectionPages.clear();
    if (result.isCancelled) return;
    const auto& selected = std::get<ClippingResult>(result.data);
    content.currentPage = content.selectionPage + selected.endPageOffset;
    content.selection = {selected.startPageOffset, selected.endPageOffset, selected.startWordIndex,
                         selected.endWordIndex, true};
    if (lookup->depth == lookup->history.size()) {
      showMessage(StrId::STR_DICT_HISTORY_FULL);
      return;
    }
    const auto heap = HalMemory::getDefaultHeap();
    if (heap.largestBlockBytes < selected.text.size() + 1 || heap.freeBytes < selected.text.size() + HEAP_HEADROOM) {
      LOG_ERR("DICT", "Low heap for lookup text (%u bytes)", static_cast<unsigned>(selected.text.size() + 1));
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      return;
    }
    lookup->pendingWord = selected.text;
  });
}

void DictionaryDefinitionActivity::loop() {
  if (initialLayoutFailed) {
    if (millis() - messageTime >= MESSAGE_DURATION_MS) finish();
    return;
  }
  if (lookup && !lookup->pendingWord.empty()) {
    navigateDefinition(false);
    return;
  }
  if (messageVisible && millis() - messageTime >= MESSAGE_DURATION_MS) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      messageVisible = false;
      requestUpdate();
    }
  }
  const int totalPages = content.totalPages;
  int sx = 0;
  int sy = 0;
  const bool longPress = lookup && mappedInput.wasScreenLongPress(sx, sy);
  const bool headerLongPress = longPress && HeaderBackTapTarget::contains(sx, sy);
  if (headerLongPress || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (lookup && (headerLongPress || mappedInput.getHeldTime() >= ReaderUtils::GO_BACK_OR_HOME_MS)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else if (lookup && lookup->depth > 0)
      navigateDefinition(true);
    else
      finish();
    return;
  }
  if (longPress && SETTINGS.touchReaderControls) {
    startSelection(sx, sy);
    return;
  }
  if (lookup && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startSelection();
    return;
  }

  // Page navigation must keep polling input while the render task refreshes the display.
  auto currentPage = std::atomic_ref(content.currentPage);
  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        haptic_feedback::touchAction();
        currentPage--;
        content.selection.valid = false;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      haptic_feedback::touchAction();
      currentPage++;
      content.selection.valid = false;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this, &currentPage] {
    if (currentPage + 1 < content.totalPages) {
      currentPage++;
      content.selection.valid = false;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this, &currentPage] {
    if (currentPage > 0) {
      currentPage--;
      content.selection.valid = false;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY,
                                            const int currentPage) const {
  const auto& pages = content.pages;
  const auto& lines = content.lines;
  const auto& definition = content.text;
  const int linesPerPage = content.linesPerPage;
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId, PLAIN_TEXT_LINE_SPACING);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = utf8SafeTruncateBuffer(definition.c_str() + lines[i].start,
                                              std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES));
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf, true,
                      lines[i].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  const int currentPage = std::atomic_ref(content.currentPage).load();
  const int totalPages = content.totalPages;
  renderer.clearScreen();

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
                 content.headword.c_str(), counter[0] ? counter : nullptr);

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const BodyArea body = bodyArea();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, body.x, body.y, currentPage);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, body.x, body.y, currentPage);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), lookup ? tr(STR_LOOKUP) : "", (currentPage > 0 ? "<" : ""),
                                            (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messageVisible) {
    GUI.drawPopup(renderer, I18N.get(message));
    return;
  }
  renderer.displayBuffer();
}
