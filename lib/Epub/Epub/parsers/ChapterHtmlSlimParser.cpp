#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryManager.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <new>

#include "../../../../src/fontIds.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// This number comes from PR #73
// If we have > 750 words buffered up, perform the layout and consume out all but the last line
// There should be enough here to build out 1-2 full pages and doing this will free up a lot of
// memory.
// Spotted when reading Intermezzo, there are some really long text blocks in there.
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS = 750;

// When CSS is enabled, flush earlier to save RAM. 320 is still more than enough to build a CJK
// page at font size 14
constexpr size_t TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS = 320;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

// Reuse serializable PageLine/PageHorizontalRule elements for a small grid.
constexpr int16_t TABLE_CELL_HORIZONTAL_PADDING = 4;
constexpr int16_t TABLE_ROW_SEPARATOR_GAP = 4;
constexpr uint8_t TABLE_ROW_SEPARATOR_THICKNESS = 1;
constexpr int16_t TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS = 3;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "ul", "ol", "pre", "figure"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* LINETHROUGH_TAGS[] = {"del", "s", "strike"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isNonVisibleTextTag(const char* name) { return VisibleTextUtils::isNonVisibleElement(name); }

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

uint16_t parseTableSpan(const char* value) {
  if (!value || value[0] == '\0') return 1;

  uint32_t span = 0;
  for (const char* current = value; *current != '\0'; ++current) {
    if (*current < '0' || *current > '9') return 1;
    const uint32_t digit = static_cast<uint32_t>(*current - '0');
    if (span > (UINT16_MAX - digit) / 10) return UINT16_MAX;
    span = span * 10 + digit;
  }
  return span == 0 ? UINT16_MAX : static_cast<uint16_t>(span);
}

// Returns true if the HTML element is a purely inline, non-navigable wrapper.
// IDs on these elements are never meaningful navigation targets in epub content.
// Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject thousands
// of such IDs for progress tracking or internal bookkeeping, and recording each one
// as a navigation anchor exhausts the heap on memory-constrained devices.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

EpdFontFamily::Style ChapterHtmlSlimParser::fontStyleForTextDecoration(const CssTextDecoration decoration) {
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  if ((decoration & CssTextDecoration::Underline) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);
  }
  if ((decoration & CssTextDecoration::LineThrough) != CssTextDecoration::None) {
    style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::STRIKETHROUGH);
  }
  return style;
}

void ChapterHtmlSlimParser::applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasTextDecoration()) {
    entry.hasTextDecoration = true;
    entry.textDecoration = css.textDecoration;
  }
}

void ChapterHtmlSlimParser::applyInlinePresentationToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.defined.whiteSpace) {
    entry.hasWhiteSpace = true;
    entry.preserveWhitespace = css.preserveWhitespace;
  }
  if (css.hasSmallCaps()) {
    entry.hasSmallCaps = true;
    entry.smallCaps = css.smallCaps;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

void ChapterHtmlSlimParser::pushBlockTextStyleEntry(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasTextDecoration() &&
      !cssStyle.hasSmallCaps() && !cssStyle.defined.whiteSpace && !cssStyle.hasDirection() &&
      !cssStyle.hasTextAlign()) {
    return;
  }

  StyleStackEntry entry;
  entry.depth = depth;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyTextDecorationToEntry(entry, cssStyle);
  applyInlinePresentationToEntry(entry, cssStyle);
  applyDirectionToEntry(entry, cssStyle);
  entry.setsParagraphDirection = true;
  if (cssStyle.hasTextAlign()) {
    entry.hasTextAlign = true;
    entry.textAlign = cssStyle.textAlign;
  }
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::pushDecorationStyleEntry(const CssTextDecoration defaultDecoration,
                                                     const CssStyle& cssStyle) {
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasTextDecoration = true;
  entry.textDecoration = cssStyle.hasTextDecoration() ? cssStyle.textDecoration : defaultDecoration;
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  applyInlinePresentationToEntry(entry, cssStyle);
  applyDirectionToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
  updateEffectiveInlineStyle();
}

// Update effective bold/italic/decorations based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveTextDecoration =
      currentCssStyle.hasTextDecoration() ? currentCssStyle.textDecoration : CssTextDecoration::None;
  effectiveSmallCaps = currentCssStyle.hasSmallCaps() && currentCssStyle.smallCaps;
  effectivePreserveWhitespace = currentCssStyle.defined.whiteSpace && currentCssStyle.preserveWhitespace;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (!blockStyleStack.empty()) {
    const auto& blockStyle = blockStyleStack.back();
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveTextAlignDefined = currentCssStyle.hasTextAlign();
  effectiveTextAlign = currentCssStyle.textAlign;
  effectiveSup = false;
  effectiveSub = false;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    // CSS line decorations propagate through descendants; child entries add
    // their own lines but cannot cancel an ancestor's already active line.
    if (entry.hasTextDecoration) {
      effectiveTextDecoration = effectiveTextDecoration | entry.textDecoration;
    }
    if (entry.hasWhiteSpace) effectivePreserveWhitespace = entry.preserveWhitespace;
    if (entry.hasSmallCaps) {
      effectiveSmallCaps = entry.smallCaps;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasTextAlign) {
      effectiveTextAlignDefined = true;
      effectiveTextAlign = entry.textAlign;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }

  // Keep flow direction in the active empty text block. Inline direction remains
  // available for CSS inheritance without replacing the paragraph's base direction.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completeCurrentPage();
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

void ChapterHtmlSlimParser::emitCurrentPage() {
  // Open boxes are sliced at the page break and continue on the next page.
  for (BoxScope& box : boxScopes) {
    emitBoxSegment(box, false);
    if (box.top >= 0) box.continued = true;
    box.top = box.bottom = -1;
  }
  completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
  recentLineCount = 0;
  keepWithNextLines = 0;
  paragraphLinesOnPage = 0;
}

void ChapterHtmlSlimParser::noteContent(const int top, const int bottom) {
  for (BoxScope& box : boxScopes) {
    if (box.top < 0) box.top = static_cast<int16_t>(top);
    box.bottom = static_cast<int16_t>(std::max<int>(box.bottom, bottom));
  }
}

void ChapterHtmlSlimParser::openBoxScope(BlockStyle& ownStyle, const CssStyle& cssStyle) {
  const bool shaded = cssStyle.defined.shaded && cssStyle.shaded;
  if (tableDepth > 0 || boxScopes.size() >= MAX_BOX_SCOPES || (!cssStyle.hasVisibleBorder() && !shaded)) return;

  BoxScope box;
  box.depth = depth;
  box.shaded = shaded;
  const CssBorderSide* sides[4] = {&cssStyle.borderTop, &cssStyle.borderRight, &cssStyle.borderBottom,
                                   &cssStyle.borderLeft};
  for (size_t i = 0; i < 4; ++i) {
    if (sides[i]->visible()) box.sides[i] = *sides[i];
  }
  const BlockStyle& parent = blockStyleStack.back();
  box.left = static_cast<int16_t>(parent.leftInset() + ownStyle.marginLeft);
  box.right = static_cast<int16_t>(viewportWidth - parent.rightInset() - ownStyle.marginRight);
  if (box.right - box.left < 8) return;

  // The border sits between margin and padding, so it joins the padding. Text also
  // keeps a small gap from the frame even when the book sets no padding.
  constexpr int16_t MIN_FRAME_GAP = 4;
  const auto pad = [&](int16_t& padding, const CssBorderSide& side) {
    if (side.width == 0 && !box.shaded) return;
    padding = static_cast<int16_t>(std::max(padding, MIN_FRAME_GAP) + side.width);
  };
  pad(ownStyle.paddingTop, box.sides[0]);
  pad(ownStyle.paddingRight, box.sides[1]);
  pad(ownStyle.paddingBottom, box.sides[2]);
  pad(ownStyle.paddingLeft, box.sides[3]);
  box.padTop = ownStyle.paddingTop;
  box.padBottom = ownStyle.paddingBottom;
  boxScopes.push_back(box);
}

void ChapterHtmlSlimParser::closeBoxScope() {
  BoxScope box = boxScopes.back();
  boxScopes.pop_back();
  if (box.top < 0 && !box.continued) {
    // An element without content (a rule drawn with borders) gets its own height.
    // Its spacing still sits on the empty text block; consume it here instead.
    const int height = box.padTop + box.padBottom;
    if (height <= 0) return;
    int above = 0;
    int below = 0;
    if (currentTextBlock && currentTextBlock->isEmpty()) {
      BlockStyle& pending = currentTextBlock->getBlockStyle();
      above = pending.marginTop;
      below = pending.marginBottom;
      pending.marginTop = pending.paddingTop = pending.marginBottom = pending.paddingBottom = 0;
    }
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }
    if (!currentPage->elements.empty() && currentPageNextY + above + height > viewportHeight) {
      completeCurrentPage();
      above = 0;
    }
    const int top = currentPageNextY + above;
    box.top = static_cast<int16_t>(top + box.padTop);
    box.bottom = box.top;
    currentPageNextY = static_cast<int16_t>(top + height + below);
    noteContent(top, top + height);
  }
  emitBoxSegment(box, true);
}

void ChapterHtmlSlimParser::emitBoxSegment(const BoxScope& box, const bool closing) {
  if (box.top < 0 || !currentPage) return;
  const int top = std::max(0, box.top - box.padTop);
  const int bottom = std::min<int>(viewportHeight, box.bottom + (closing ? box.padBottom : 0));
  if (bottom <= top) return;
  // A page break slices the box: the continuing edges are left open.
  const CssBorderSide sides[4] = {box.continued ? CssBorderSide{} : box.sides[0], box.sides[1],
                                  closing ? box.sides[2] : CssBorderSide{}, box.sides[3]};
  auto element =
      makeUniqueNoThrow<PageBorderBox>(static_cast<uint16_t>(box.right - box.left), static_cast<uint16_t>(bottom - top),
                                       sides, box.shaded, box.left, static_cast<int16_t>(top));
  if (!element) {
    LOG_ERR("EHP", "OOM: PageBorderBox");
    return;
  }
  currentPage->elements.push_back(std::move(element));
}

uint8_t ChapterHtmlSlimParser::dropCapLines(const CssStyle& style) {
  static_assert(MAX_DROP_CAP_BYTES == PageDropCap::MAX_TEXT_BYTES, "captured drop-cap text must fit a PageDropCap");
  if (style.defined.initialLetter && style.initialLetter > 0) {
    return style.initialLetter >= 2 ? style.initialLetter : 0;
  }
  const bool large = style.hasFontSize() && style.fontSize.value >= 1.5f;
  if (!large && !(style.defined.floatLeft && style.floatLeft)) return 0;
  // A floated letter without a size spans three lines; a sized one about its size in ems.
  return large ? static_cast<uint8_t>(std::clamp(static_cast<int>(style.fontSize.value + 0.5f), 2, 4)) : 3;
}

void ChapterHtmlSlimParser::armFirstLetterDropCap(const char* tagName, const std::string& classAttr,
                                                  const char* idAttr) {
  if (!cssParser || !cssParser->hasFirstLetterRules() || tableDepth > 0 || dropCap.length > 0) return;
  const auto elementDepth = static_cast<size_t>(depth);
  const CssStyle firstLetter = cssParser->resolveStyle(tagName, classAttr, idAttr, cssAncestors.data(),
                                                       elementDepth <= MAX_CSS_ANCESTORS ? elementDepth : 0, true);
  const uint8_t lines = dropCapLines(firstLetter);
  if (lines == 0) return;
  dropCap.firstLetterPending = true;
  dropCap.lines = lines;
  dropCap.bold = firstLetter.hasFontWeight() && firstLetter.fontWeight == CssFontWeight::Bold;
}

namespace {
// Opening quotes and other punctuation that lead into a drop cap letter.
bool isDropCapPunctuation(const uint32_t cp) {
  if (cp < 0x80) return std::ispunct(static_cast<int>(cp)) != 0;
  return cp == 0xA1 || cp == 0xAB || cp == 0xBB || cp == 0xBF || (cp >= 0x2010 && cp <= 0x205E) ||
         (cp >= 0x3008 && cp <= 0x3011);
}
}  // namespace

// Returns false (and restores the captured text to the word buffer) once the
// capture is too long to be an initial letter.
bool ChapterHtmlSlimParser::captureDropCapCodepoint(const char* bytes, const int length) {
  constexpr uint8_t MAX_DROP_CAP_CODEPOINTS = 3;
  if (dropCap.length + length > static_cast<int>(MAX_DROP_CAP_BYTES) || dropCap.codepoints >= MAX_DROP_CAP_CODEPOINTS) {
    cancelDropCapToWord();
    return false;
  }
  memcpy(dropCap.text + dropCap.length, bytes, static_cast<size_t>(length));
  dropCap.length = static_cast<uint8_t>(dropCap.length + length);
  dropCap.text[dropCap.length] = '\0';
  dropCap.codepoints++;
  if (dropCap.spanDepth < 0) {
    // ::first-letter takes leading punctuation plus the first letter.
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes);
    if (!isDropCapPunctuation(utf8NextCodepoint(&cursor))) dropCap.firstLetterPending = false;
  }
  return true;
}

void ChapterHtmlSlimParser::cancelDropCapToWord() {
  memcpy(partWordBuffer + partWordBufferIndex, dropCap.text, dropCap.length);
  partWordBufferIndex += dropCap.length;
  dropCap = DropCapState{};
}

void ChapterHtmlSlimParser::layoutCurrentBlock(const bool includeLastLine) {
  const int layoutFontId = prepareBlockFont();
  const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  bool topSpacingApplied = wordsExtractedInBlock != 0;
  const auto applyTopSpacing = [this, &topSpacingApplied]() {
    if (topSpacingApplied) return;
    const auto& style = currentTextBlock->getBlockStyle();
    if (style.marginTop > 0) currentPageNextY += style.marginTop;
    if (style.paddingTop > 0) currentPageNextY += style.paddingTop;
    topSpacingApplied = true;
  };
  const auto emitLine = [this, &applyTopSpacing](std::unique_ptr<TextBlock> textBlock, const uint32_t offset) {
    applyTopSpacing();
    addLineToPage(std::move(textBlock), offset);
  };
  layoutParagraph = currentTextBlock.get();
  if (dropCap.length > 0 && dropCap.spanDepth < 0 && !currentTextBlock->isEmpty()) {
    // Lines beside the letter must stay on its page.
    layoutParagraphHasDropCap = true;
    applyTopSpacing();
    layoutDropCapLines(layoutFontId, effectiveWidth, emitLine, includeLastLine);
  }
  currentTextBlock->layoutAndExtractLines(renderer, layoutFontId, effectiveWidth, emitLine, includeLastLine,
                                          characterSpacing, wordSpacingPercent);
  layoutParagraph = nullptr;
}

void ChapterHtmlSlimParser::layoutDropCapLines(
    const int layoutFontId, const uint16_t effectiveWidth,
    const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& emitLine, const bool includeLastLine) {
  const uint8_t lines = dropCap.lines;
  char text[MAX_DROP_CAP_BYTES + 1];
  memcpy(text, dropCap.text, sizeof(text));
  const EpdFontFamily::Style capStyle = dropCap.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  dropCap = DropCapState{};

  // Enlarging the family's largest built-in size keeps outlines smoothest.
  const int capFontId = fontIdForScale(3.0f);
  const int lineHeight = renderer.getLineHeight(layoutFontId, lineCompression);
  const int ascender = renderer.getFontAscenderSize(layoutFontId);
  int32_t advanceFP = 0;
  int bodyCapTop = 0;
  int sourceCapTop = 0;
  if (!renderer.getCodepointMetrics(layoutFontId, 'H', EpdFontFamily::REGULAR, advanceFP, bodyCapTop)) {
    bodyCapTop = ascender * 7 / 10;
  }
  if (!renderer.getCodepointMetrics(capFontId, 'H', capStyle, advanceFP, sourceCapTop) || sourceCapTop <= 0) {
    sourceCapTop = bodyCapTop > 0 ? bodyCapTop : 1;
  }
  // The letter's top aligns with the first line's capitals, its baseline with the last spanned line.
  const int capHeight = (lines - 1) * lineHeight + bodyCapTop;
  const int scale256 = std::clamp(capHeight * 256 / sourceCapTop, 256, 4096);

  int capWidth = renderer.getSpaceWidth(layoutFontId, EpdFontFamily::REGULAR);
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    int top = 0;
    if (renderer.getCodepointMetrics(capFontId, cp, capStyle, advanceFP, top)) {
      capWidth += fp4::toPixel(advanceFP * scale256 / 256);
    }
  }
  capWidth = std::min(capWidth, effectiveWidth / 2);

  // Keep every spanned line on the letter's page.
  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }
  if (!currentPage->elements.empty() && currentPageNextY + lines * lineHeight > viewportHeight) {
    breakPageCarryingLines(keepWithNextCarry(), 0, visibleTextOffset);
  }
  BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int top = currentPageNextY;
  auto element =
      makeUniqueNoThrow<PageDropCap>(capFontId, static_cast<uint16_t>(scale256), capStyle, text, blockStyle.leftInset(),
                                     static_cast<int16_t>(top + (lines - 1) * lineHeight + ascender));
  if (element) {
    currentPage->elements.push_back(std::move(element));
    noteContent(top, top + lines * lineHeight);
  } else {
    LOG_ERR("EHP", "OOM: PageDropCap");
  }

  // The spanned lines wrap beside the letter; a drop-cap paragraph has no first-line indent.
  currentTextBlock->suppressFirstLineIndent();
  blockStyle.textIndent = 0;
  blockStyle.textIndentDefined = true;
  blockStyle.marginLeft = static_cast<int16_t>(blockStyle.marginLeft + capWidth);
  currentTextBlock->layoutAndExtractLines(renderer, layoutFontId, static_cast<uint16_t>(effectiveWidth - capWidth),
                                          emitLine, includeLastLine, characterSpacing, wordSpacingPercent, lines);
  blockStyle.marginLeft = static_cast<int16_t>(blockStyle.marginLeft - capWidth);
  currentPageNextY = static_cast<int16_t>(std::max(static_cast<int>(currentPageNextY), top + lines * lineHeight));
}

void ChapterHtmlSlimParser::completeCurrentPage() {
  emitCurrentPage();
  completedPageCount++;
  currentPage.reset(new Page());
  currentPageNextY = 0;
  currentPageVisibleOffsetSet = false;
}

// Honors a pending page-break-after and the current block's page-break-before,
// once, before the block (or image) places its first content.
void ChapterHtmlSlimParser::applyPendingPageBreak() {
  bool requested = pendingPageBreak;
  pendingPageBreak = false;
  if (currentTextBlock) {
    BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
    requested = requested || blockStyle.pageBreakBefore;
    blockStyle.pageBreakBefore = false;
  }
  if (requested && currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
  }
}

void ChapterHtmlSlimParser::pushBlockStyle(const BlockStyle& accumulated) {
  // The break belongs to the element's first block only; blocks restarted from
  // the stack after a child closes must not break again.
  BlockStyle entry = accumulated;
  entry.pageBreakBefore = false;
  blockStyleStack.push_back(entry);
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  // The first page always begins at the start of the body, even when the XHTML
  // contains leading formatting whitespace before its first rendered word.
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Block creation failed (OOM): drop the buffered text; parseStep() is about
  // to fail the build via layoutOom.
  if (!currentTextBlock) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  if (effectivePreserveWhitespace) {
    currentTextBlock->suppressFirstLineIndent();
    auto& style = currentTextBlock->getBlockStyle();
    if (style.alignment == CssTextAlign::Justify) style.alignment = CssTextAlign::Left;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | fontStyleForTextDecoration(effectiveTextDecoration));
  if (effectiveSmallCaps) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SMALL_CAPS);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  const size_t wordBytes = static_cast<size_t>(partWordBufferIndex);
  if (insideTableCell && !tableRowStacked && tableRowTextBytes + wordBytes > MAX_GRID_TABLE_ROW_BYTES) {
    fallbackTableRowToStacked();
  }

  uint8_t linkId = 0;
  if (insideFootnoteLink) {
    if (!currentTextBlock->linkTargetMatches(currentFootnoteLinkId, currentFootnote.href)) {
      currentFootnoteLinkId = currentTextBlock->addLinkTarget(currentFootnote.href);
    }
    linkId = currentFootnoteLinkId;
  }
  // Inline font-size spans size their words apart from the block.
  float inlineScale = 0.0f;
  for (const auto& entry : inlineStyleStack) {
    if (!entry.hasFontScale) continue;
    const float base = inlineScale > 0.0f ? inlineScale : currentTextBlock->getBlockStyle().fontScale;
    inlineScale = entry.fontScaleRem ? entry.fontScale : base * entry.fontScale;
  }
  const uint8_t sizeSlot = inlineScale > 0.0f ? currentTextBlock->sizeSlotFor(std::clamp(inlineScale, 0.5f, 3.0f)) : 0;
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, partWordVisibleOffset, linkId,
                            sizeSlot, effectivePreserveWhitespace);
  if (insideTableCell && !tableRowStacked) {
    tableRowTextBytes += wordBytes;
    size_t rowWords = currentTextBlock->size();
    for (const auto& cell : tableRowCells) rowWords += cell->size();
    if (rowWords > MAX_GRID_TABLE_ROW_WORDS) {
      fallbackTableRowToStacked();
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  listItemBulletOnly = false;
}

void ChapterHtmlSlimParser::breakTextLine(const BlockStyle& style) {
  const bool hasText = currentTextBlock && !currentTextBlock->isEmpty();
  if (!hasText && currentPage && !currentPage->elements.empty()) {
    const int lineHeight = renderer.getLineHeight(fontId, lineCompression);
    currentPageNextY = static_cast<int16_t>(std::min<int>(viewportHeight, currentPageNextY + lineHeight));
  }
  startNewTextBlock(style, false);
  if (currentTextBlock && (hasText || effectivePreserveWhitespace)) currentTextBlock->suppressFirstLineIndent();
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle, const bool paragraphEnd) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      if (paragraphEnd) currentTextBlock->resetFirstLineIndent();
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      inlineSize = InlineSizeState{};

      flushPendingAnchor();
      return;
    }

    // <li> added a bullet as the first word, making the block non-empty. When a nested
    // block-level child (<p>, <div>, etc.) opens, reuse the block instead of flushing
    // the bullet to its own line. The bullet stays inline with the child's text.
    if (listItemBulletOnly) {
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      listItemBulletOnly = false;
      flushPendingAnchor();
      return;
    }

    makePages(true, paragraphEnd);
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  dropCap.firstLetterPending = false;
  inlineSize = InlineSizeState{};
  currentTextBlock =
      makeUniqueNoThrow<ParsedText>(hyphenationEnabled, focusReadingEnabled, blockStyle, paragraphIndentSpaces);
  if (!currentTextBlock) {
    // Evict rebuildable caches and retry once before failing the build.
    freeink::MemoryManager::instance().ensureFree(4 * 1024);
    currentTextBlock =
        makeUniqueNoThrow<ParsedText>(hyphenationEnabled, focusReadingEnabled, blockStyle, paragraphIndentSpaces);
  }
  if (!currentTextBlock) {
    LOG_ERR("EHP", "OOM: ParsedText");
    layoutOom = true;  // parseStep() turns this into ParseStatus::Error
  }
  wordsExtractedInBlock = 0;
  listItemBulletOnly = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId, lineCompression));
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleTextOffset);
    emitCurrentPage();
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  currentPageNextY += topSpacing;

  auto pageRule = makeUniqueNoThrow<PageHorizontalRule>(width, ruleThickness, xPos, currentPageNextY);
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(std::move(pageRule));
  setCurrentPageVisibleOffset(visibleTextOffset);
  noteContent(currentPageNextY, currentPageNextY + ruleThickness);
  keepWithNextLines = 0;
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::fallbackTableRowToStacked() {
  if (tableRowStacked) {
    return;
  }

  auto activeCell = std::move(currentTextBlock);
  tableRowStacked = true;

  for (auto& cell : tableRowCells) {
    currentTextBlock = std::move(cell);
    wordsExtractedInBlock = 0;
    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      makePages();
    }
  }
  tableRowCells.clear();
  currentTextBlock = std::move(activeCell);
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::closeTableCell() {
  if (!insideTableCell) {
    return;
  }
  insideTableCell = false;
  const int bottomPadding = blockStyleStack.back().paddingBottom;
  blockStyleStack.pop_back();

  if (!currentTextBlock) {
    return;
  }

  // Latch before the cell leaves currentTextBlock: parseStep()'s dropped-word
  // check only inspects currentTextBlock, so a cell parsed and moved (or reset
  // while empty) within one XML buffer would otherwise lose its OOM flag.
  if (currentTextBlock->hadDroppedWords()) {
    layoutOom = true;
  }

  if (!tableRowStacked && tableRowCells.size() >= MAX_GRID_TABLE_COLUMNS) {
    fallbackTableRowToStacked();
  }

  if (tableRowStacked) {
    wordsExtractedInBlock = 0;
    if (!currentTextBlock->isEmpty()) {
      makePages();
    } else {
      currentPageNextY += bottomPadding;
    }
    currentTextBlock.reset();
    return;
  }

  tableRowCells.push_back(std::move(currentTextBlock));
}

void ChapterHtmlSlimParser::addTableRowSeparator() {
  if (!currentPage || currentPage->elements.empty() || viewportWidth == 0 ||
      currentPageNextY + TABLE_ROW_SEPARATOR_GAP > viewportHeight) {
    return;
  }

  auto separator =
      makeUniqueNoThrow<PageHorizontalRule>(viewportWidth, TABLE_ROW_SEPARATOR_THICKNESS, 0, currentPageNextY + 1);
  if (!separator) {
    LOG_ERR("EHP", "OOM: table row separator");
    return;
  }
  if (currentPage->elements.capacity() == currentPage->elements.size()) {
    currentPage->elements.reserve(currentPage->elements.size() + 1);
  }
  currentPage->elements.push_back(std::move(separator));
  currentPageNextY += TABLE_ROW_SEPARATOR_GAP;
}

// Streams a table's raw markup from its opening tag and measures each column's
// longest word and longest unwrapped line, so every row can share one set of
// widths. Returns false when the table is too long or too wide to plan.
bool ChapterHtmlSlimParser::measureTableColumns(HalFile& file, TableColumnMeasure& out) const {
  out = TableColumnMeasure{};
  constexpr size_t MAX_SCAN_BYTES = 32 * 1024;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);

  char buffer[96];
  char word[40];
  size_t wordLength = 0;
  int wordWidth = 0;  // flushed chunks of a word longer than the buffer
  char tagText[48];
  size_t tagLength = 0;
  bool inTag = false;
  bool inEntity = false;
  char quote = 0;
  int tableNesting = 0;
  int column = -1;
  bool inCell = false;
  bool skipCell = false;
  int lineWidth = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;

  const auto flushChunk = [&] {
    if (wordLength == 0) return;
    word[wordLength] = '\0';
    wordWidth += renderer.getTextAdvanceX(fontId, word, style);
    wordLength = 0;
  };
  const auto finishWord = [&] {
    flushChunk();
    if (wordWidth == 0 || column < 0) return;
    out.minWidth[column] = static_cast<uint16_t>(std::max<int>(out.minWidth[column], wordWidth));
    lineWidth += (lineWidth > 0 ? spaceWidth : 0) + wordWidth;
    wordWidth = 0;
  };
  const auto finishLine = [&] {
    finishWord();
    if (column >= 0) out.prefWidth[column] = static_cast<uint16_t>(std::max<int>(out.prefWidth[column], lineWidth));
    lineWidth = 0;
  };
  // Returns 1 when the table ends, -1 when it cannot be planned, 0 otherwise.
  const auto handleTag = [&]() -> int {
    tagText[tagLength] = '\0';
    const bool closing = tagText[0] == '/';
    const char* name = tagText + (closing ? 1 : 0);
    size_t nameLength = 0;
    while (name[nameLength] && !isWhitespace(name[nameLength]) && name[nameLength] != '/') ++nameLength;
    const std::string_view tag(name, nameLength);
    if (tag == "table") {
      tableNesting += closing ? -1 : 1;
      if (tableNesting == 0) return 1;
      return 0;
    }
    if (tableNesting != 1) {
      finishWord();
      return 0;
    }
    if (tag == "tr" && !closing) {
      if (inCell) finishLine();
      inCell = false;
      column = -1;
    } else if (tag == "td" || tag == "th") {
      if (inCell) finishLine();
      inCell = !closing;
      if (!closing) {
        // Spanning cells lay out as stacked rows, so they don't shape the grid.
        skipCell = strstr(tagText, "colspan") != nullptr || strstr(tagText, "rowspan") != nullptr;
        if (++column >= static_cast<int>(MAX_GRID_TABLE_COLUMNS)) return -1;
        out.columns = static_cast<uint8_t>(std::max<int>(out.columns, column + 1));
        style = tag == "th" ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      }
    } else if (tag == "br") {
      finishLine();
    } else if (tag == "p" || tag == "div" || tag == "li" || (tag.size() == 2 && tag[0] == 'h')) {
      finishWord();  // block markup inside a cell collapses to a word boundary
    }
    return 0;
  };

  size_t scanned = 0;
  while (scanned < MAX_SCAN_BYTES) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (count == 0) return false;
    scanned += count;
    for (size_t i = 0; i < count; ++i) {
      const char c = buffer[i];
      if (inTag) {
        if (quote) {
          if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
          quote = c;
        } else if (c == '>') {
          inTag = false;
          const int result = handleTag();
          if (result != 0) return result > 0 && out.columns >= 2;
          continue;
        }
        if (tagLength + 1 < sizeof(tagText)) {
          tagText[tagLength++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        continue;
      }
      if (c == '<') {
        inTag = true;
        tagLength = 0;
        continue;
      }
      if (!inCell || skipCell || tableNesting != 1) continue;
      if (inEntity) {
        inEntity = c != ';';
        continue;
      }
      if (isWhitespace(c)) {
        finishWord();
        continue;
      }
      if (wordLength + 1 >= sizeof(word)) flushChunk();
      // An entity measures roughly as one character.
      inEntity = c == '&';
      word[wordLength++] = inEntity ? 'n' : c;
    }
  }
  return false;
}

// Distributes the viewport across columns the way CSS auto table layout does:
// natural widths when they fit, otherwise each column keeps its longest word and
// shares the rest in proportion to how much wrapping it would otherwise need.
void ChapterHtmlSlimParser::planTableColumns(const TableColumnMeasure& measure) {
  tableColumnCount = 0;
  const int columns = measure.columns;
  if (columns < 2 || columns > static_cast<int>(MAX_GRID_TABLE_COLUMNS)) return;
  constexpr int padding = TABLE_CELL_HORIZONTAL_PADDING * 2;
  const int available = viewportWidth;
  int minWidth[MAX_GRID_TABLE_COLUMNS];
  int prefWidth[MAX_GRID_TABLE_COLUMNS];
  int minTotal = 0;
  int prefTotal = 0;
  for (int c = 0; c < columns; ++c) {
    minWidth[c] = measure.minWidth[c] + padding;
    prefWidth[c] = std::max(minWidth[c], measure.prefWidth[c] + padding);
    minTotal += minWidth[c];
    prefTotal += prefWidth[c];
  }
  int assigned = 0;
  for (int c = 0; c < columns; ++c) {
    int width;
    if (prefTotal <= available) {
      width = prefWidth[c] + (available - prefTotal) * prefWidth[c] / prefTotal;
    } else if (minTotal <= available) {
      width = minWidth[c] + (available - minTotal) * (prefWidth[c] - minWidth[c]) / (prefTotal - minTotal);
    } else {
      width = available * minWidth[c] / minTotal;
    }
    if (c + 1 == columns) width = available - assigned;
    tableColumnWidths[c] = static_cast<uint16_t>(std::max(width, padding + 1));
    assigned += width;
  }
  tableColumnCount = static_cast<uint8_t>(columns);
}

void ChapterHtmlSlimParser::finishTableRow() {
  closeTableCell();

  if (tableRowCells.empty()) {
    if (tableRowStacked) {
      addTableRowSeparator();
    }
    tableRowStacked = false;
    return;
  }

  const int16_t lineHeight =
      std::max<int16_t>(1, static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression));
  const size_t columnCount = tableRowCells.size();
  const bool planned = tableColumnCount == columnCount;
  uint16_t columnWidths[MAX_GRID_TABLE_COLUMNS] = {};
  bool fitsGrid = columnCount >= 2;
  int16_t topPadding[MAX_GRID_TABLE_COLUMNS] = {};
  int maxTopPadding = 0;
  int maxBottomPadding = 0;
  for (size_t column = 0; column < columnCount; ++column) {
    columnWidths[column] = planned ? tableColumnWidths[column] : static_cast<uint16_t>(viewportWidth / columnCount);
    // Equal columns keep enough width for a few glyphs so ordinary three-column tables stay
    // tabular in portrait; planned columns already fit their longest word.
    const auto& style = tableRowCells[column]->getBlockStyle();
    topPadding[column] = style.paddingTop;
    maxTopPadding = std::max<int>(maxTopPadding, style.paddingTop);
    maxBottomPadding = std::max<int>(maxBottomPadding, style.paddingBottom);
    fitsGrid = fitsGrid && columnWidths[column] > style.paddingLeft + style.paddingRight &&
               (planned || columnWidths[column] >= lineHeight * TABLE_MIN_CELL_WIDTH_LINE_HEIGHTS);
  }
  if (!fitsGrid) {
    fallbackTableRowToStacked();
    addTableRowSeparator();
    tableRowStacked = false;
    return;
  }
  const bool rowIsRtl = tableRowRtl;
  // Left edge of a logical column; RTL rows mirror the column order.
  const auto columnX = [&](const size_t column) {
    int x = 0;
    for (size_t other = 0; other < columnCount; ++other) {
      if (rowIsRtl ? other > column : other < column) x += columnWidths[other];
    }
    return x;
  };
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();
  size_t maxLineCount = 0;

  for (size_t column = 0; column < columnCount; ++column) {
    auto& lines = tableCellLines[column];
    // Reserve from this cell's size, rather than the full row budget for every column.
    const size_t estimatedLines = tableRowCells[column]->size() * 2;
    lines.reserve(estimatedLines);
    tableLineVisibleOffsets.reserve(estimatedLines);
    tableRowCells[column]->layoutAndExtractLines(
        renderer, fontId,
        static_cast<uint16_t>(columnWidths[column] - tableRowCells[column]->getBlockStyle().paddingLeft -
                              tableRowCells[column]->getBlockStyle().paddingRight),
        [this, &lines](std::unique_ptr<TextBlock> line, const uint32_t offset) {
          const size_t lineIndex = lines.size();
          lines.push_back(std::move(line));
          if (tableLineVisibleOffsets.size() <= lineIndex) {
            tableLineVisibleOffsets.resize(lineIndex + 1, UINT32_MAX);
          }
          tableLineVisibleOffsets[lineIndex] = std::min(tableLineVisibleOffsets[lineIndex], offset);
        },
        true, characterSpacing, wordSpacingPercent);
    maxLineCount = std::max(maxLineCount, lines.size());
  }
  // Cell layout itself can drop lines (TextBlock arena OOM in extractLine);
  // latch that before the cells are destroyed.
  for (const auto& cell : tableRowCells) {
    if (cell && cell->hadDroppedWords()) layoutOom = true;
  }
  tableRowCells.clear();
  const auto clearLayoutLines = [this]() {
    for (auto& lines : tableCellLines) {
      lines.clear();
    }
    tableLineVisibleOffsets.clear();
  };

  // Bordered tables frame every cell. Boxes overlap their neighbours by one pixel so
  // shared edges draw as a single line; a row split by a page break is framed per page.
  CssBorderSide cellBorder = tableBorder;
  cellBorder.width = std::min<uint8_t>(cellBorder.width, 3);
  const bool bordered = cellBorder.visible();
  const int16_t cellGap = bordered ? static_cast<int16_t>(cellBorder.width + 2) : 0;
  int sliceTop = -1;
  const auto frameCells = [&](const int top, const int bottom) {
    const CssBorderSide sides[4] = {cellBorder, cellBorder, cellBorder, cellBorder};
    const int height = std::min(bottom + 1, static_cast<int>(viewportHeight)) - top;
    if (height <= 0) return;
    for (size_t column = 0; column < columnCount; ++column) {
      const int x = columnX(column);
      const int width = x + columnWidths[column] >= viewportWidth ? viewportWidth - x : columnWidths[column] + 1;
      auto box = makeUniqueNoThrow<PageBorderBox>(static_cast<uint16_t>(width), static_cast<uint16_t>(height), sides,
                                                  false, static_cast<int16_t>(x), static_cast<int16_t>(top));
      if (!box) {
        LOG_ERR("EHP", "OOM: table cell border");
        return;
      }
      currentPage->elements.push_back(std::move(box));
    }
  };

  for (size_t lineIndex = 0; lineIndex < maxLineCount; ++lineIndex) {
    const uint32_t lineVisibleOffset =
        lineIndex < tableLineVisibleOffsets.size() ? tableLineVisibleOffsets[lineIndex] : visibleTextOffset;
    int16_t rowLineHeight = lineHeight;
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex < tableCellLines[column].size()) {
        rowLineHeight = std::max<int16_t>(
            rowLineHeight, static_cast<int16_t>(lineHeight + tableCellLines[column][lineIndex]->getRubyShift(
                                                                 renderer.getFontAscenderSize(fontId))));
      }
    }

    const int topGap = sliceTop < 0 ? maxTopPadding + cellGap : 0;
    const int bottomGap = maxBottomPadding + cellGap;
    const bool pageFull = currentPage && !currentPage->elements.empty() &&
                          currentPageNextY + topGap + rowLineHeight + bottomGap > viewportHeight;
    if (!currentPage || pageFull) {
      if (pageFull) {
        if (bordered && sliceTop >= 0) frameCells(sliceTop, currentPageNextY + bottomGap);
        sliceTop = -1;
        setCurrentPageVisibleOffset(lineVisibleOffset);
        emitCurrentPage();
        completedPageCount++;
      }
      currentPage = makeUniqueNoThrow<Page>();
      if (!currentPage) {
        LOG_ERR("EHP", "OOM: page for table row");
        clearLayoutLines();
        return;
      }
      currentPageNextY = 0;
      currentPageVisibleOffsetSet = false;
    }

    if (sliceTop < 0) {
      sliceTop = currentPageNextY;
      currentPageNextY = static_cast<int16_t>(currentPageNextY + cellGap + maxTopPadding);
    }
    const int16_t rowY = currentPageNextY;
    const size_t requiredCapacity = currentPage->elements.size() + columnCount;
    if (currentPage->elements.capacity() < requiredCapacity) {
      const size_t linesThatFit =
          std::max<size_t>(1, static_cast<size_t>((viewportHeight - currentPageNextY) / rowLineHeight));
      const size_t linesToReserve = std::min(maxLineCount - lineIndex, linesThatFit);
      currentPage->elements.reserve(currentPage->elements.size() + linesToReserve * columnCount + 1);
    }
    for (size_t column = 0; column < columnCount; ++column) {
      if (lineIndex >= tableCellLines[column].size()) {
        continue;
      }

      auto& line = tableCellLines[column][lineIndex];
      auto style = line->getBlockStyle();
      style.marginLeft = static_cast<int16_t>(columnX(column));
      line->setBlockStyle(style);

      currentPageNextY = static_cast<int16_t>(rowY - maxTopPadding + topPadding[column]);
      addLineToPage(std::move(line), lineVisibleOffset);
    }
    currentPageNextY = static_cast<int16_t>(rowY + rowLineHeight);
  }

  currentPageNextY = static_cast<int16_t>(std::min<int>(currentPageNextY + maxBottomPadding, viewportHeight));
  keepWithNextLines = 0;
  if (bordered && sliceTop >= 0 && currentPage) {
    currentPageNextY = static_cast<int16_t>(std::min<int>(currentPageNextY + cellGap, viewportHeight));
    frameCells(sliceTop, currentPageNextY);
  } else {
    addTableRowSeparator();
  }
  tableRowStacked = false;
  clearLayoutLines();
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (strcasecmp(name, "body") == 0) {
    // Case-insensitive to match ParagraphStreamer's tag matching (ProgressMapper). A case
    // mismatch here would leave visibleTextOffset at 0 for the whole section, so every page
    // would record offset 0 while the sync resolver still counts a non-zero offset.
    self->insideBody = true;
  }
  if (self->insideBody && (self->nonVisibleTextDepth > 0 || isNonVisibleTextTag(name))) {
    self->nonVisibleTextDepth++;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, id, dir and hidden attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  const char* idAttr = "";
  bool hasHiddenAttr = false;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        //
        // Skip IDs on non-navigable inline elements (e.g. <span>): these are never
        // link targets in epub content, but reading-system converters can inject tens
        // of thousands of them per chapter, exhausting the heap. TOC anchors are
        // always recorded regardless of element type, since they drive page breaks.
        const char* idValue = atts[i + 1];
        idAttr = idValue;
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush a displaced anchor before overwriting. Consecutive non-block elements
          // (e.g. <aside id="fn1">text</aside><aside id="fn2">) with no intervening block
          // never trigger startNewTextBlock, so fn1 gets silently overwritten. That leaves
          // fn1 missing from the anchor map -> getPageForAnchor returns nullopt -> reader
          // lands at page 0 (section start) instead of the footnote.
          if (!self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      } else if (strcmp(atts[i], "hidden") == 0) {
        hasHiddenAttr = true;
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    const auto elementDepth = static_cast<size_t>(self->depth);
    cssStyle = self->cssParser->resolveStyle(name, classAttr, idAttr, self->cssAncestors.data(),
                                             elementDepth <= MAX_CSS_ANCESTORS ? elementDepth : 0);
    if (elementDepth < MAX_CSS_ANCESTORS) {
      self->cssAncestors[elementDepth] = CssParser::makeAncestor(name, classAttr, idAttr);
    }
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  if (strcmp(name, "pre") == 0 && !cssStyle.defined.whiteSpace) {
    cssStyle.preserveWhitespace = true;
    cssStyle.defined.whiteSpace = 1;
  }

  // HTML hidden attribute overrides CSS display.
  if (hasHiddenAttr) {
    cssStyle.display = CssDisplay::None;
    cssStyle.defined.display = 1;
  }

  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  // Direction is inherited in HTML/CSS. If this element does not define one, carry
  // the currently active inherited direction into its computed style.
  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffer one simple row; oversized rows fall back to full-width flow.
  if (strcmp(name, "table") == 0) {
    // Flatten nested content without allocating a recursive row buffer.
    if (self->tableDepth > 0) {
      if (self->tableDepth == 1 && self->insideTableCell && self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
      self->currentTextBlock.reset();
    }
    self->flushPendingAnchor();
    self->inlineSize = InlineSizeState{};
    self->dropCap.firstLetterPending = false;
    self->pushBlockTextStyleEntry(cssStyle);
    self->tableDepth = 1;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    self->tableRowsSpannedRemaining = 0;
    self->tableRowTextBytes = 0;
    self->tableRowCells.clear();
    self->tableRowCells.reserve(MAX_GRID_TABLE_COLUMNS);
    self->tableBorder = CssBorderSide{};
    self->tableColumnCount = 0;
    if (self->xmlParser_) {
      // Rows stream in one at a time, so plan shared column widths from the raw markup first.
      HalFile tableFile;
      TableColumnMeasure measure;
      if (Storage.openFileForRead("EHP", self->filepath, tableFile) &&
          tableFile.seek(static_cast<size_t>(XML_GetCurrentByteIndex(self->xmlParser_))) &&
          self->measureTableColumns(tableFile, measure)) {
        self->planTableColumns(measure);
      }
    }
    for (const CssBorderSide* side :
         {&cssStyle.borderTop, &cssStyle.borderRight, &cssStyle.borderBottom, &cssStyle.borderLeft}) {
      if (!self->tableBorder.visible() && side->visible()) self->tableBorder = *side;
    }
    const char* borderAttr = getAttribute(atts, "border");
    if (!self->tableBorder.visible() && borderAttr && atoi(borderAttr) > 0) {
      self->tableBorder = CssBorderSide{1, CssBorderStyle::Solid};
    }
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      // Text before the first row is typically a <caption>.
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableRowTextBytes = 0;
    self->tableRowStacked = self->tableRowsSpannedRemaining > 0;
    self->tableRowRtl = cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl;
    if (self->tableRowsSpannedRemaining != UINT16_MAX && self->tableRowsSpannedRemaining > 0) {
      self->tableRowsSpannedRemaining--;
    }
    self->pushBlockTextStyleEntry(cssStyle);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->closeTableCell();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();

    const uint16_t columnSpan = parseTableSpan(getAttribute(atts, "colspan"));
    const uint16_t rowSpan = parseTableSpan(getAttribute(atts, "rowspan"));
    if (columnSpan > 1 || rowSpan > 1) {
      self->fallbackTableRowToStacked();
    }
    if (rowSpan > 1) {
      const uint16_t remaining = rowSpan == UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(rowSpan - 1);
      self->tableRowsSpannedRemaining = std::max(self->tableRowsSpannedRemaining, remaining);
    }

    auto tableCellBlockStyle = BlockStyle();
    const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    // Bound author padding so even a stacked cell leaves room for content.
    const auto padding = [&](const CssLength& length, const int limit) {
      return static_cast<int16_t>(std::clamp<int>(length.toPixelsInt16(emSize, self->viewportWidth), 0, limit));
    };
    tableCellBlockStyle.paddingLeft = cssStyle.hasPaddingLeft() ? padding(cssStyle.paddingLeft, self->viewportWidth / 4)
                                                                : TABLE_CELL_HORIZONTAL_PADDING;
    tableCellBlockStyle.paddingRight = cssStyle.hasPaddingRight()
                                           ? padding(cssStyle.paddingRight, self->viewportWidth / 4)
                                           : TABLE_CELL_HORIZONTAL_PADDING;
    tableCellBlockStyle.paddingTop = padding(cssStyle.paddingTop, self->viewportHeight / 4);
    tableCellBlockStyle.paddingBottom = padding(cssStyle.paddingBottom, self->viewportHeight / 4);
    tableCellBlockStyle.textAlignDefined = true;
    tableCellBlockStyle.alignment =
        cssStyle.hasTextAlign()
            ? cssStyle.textAlign
            : (self->effectiveTextAlignDefined
                   ? self->effectiveTextAlign
                   : (cssStyle.hasDirection() && cssStyle.direction == CssTextDirection::Rtl ? CssTextAlign::Right
                                                                                             : CssTextAlign::Left));
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.directionDefined = true;
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
    }

    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->hyphenationEnabled, self->focusReadingEnabled,
                                                           tableCellBlockStyle, self->paragraphIndentSpaces);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: table cell");
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
    for (const CssBorderSide* side :
         {&cssStyle.borderTop, &cssStyle.borderRight, &cssStyle.borderBottom, &cssStyle.borderLeft}) {
      if (!self->tableBorder.visible() && side->visible()) self->tableBorder = *side;
    }
    self->pushBlockStyle(tableCellBlockStyle);
    self->insideTableCell = true;
    self->wordsExtractedInBlock = 0;
    self->flushPendingAnchor();
    self->pushBlockTextStyleEntry(cssStyle);

    if (strcmp(name, "th") == 0 && (!cssStyle.hasFontWeight() || cssStyle.fontWeight == CssFontWeight::Bold)) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }

    self->depth += 1;
    return;
  }

  if (self->tableDepth >= 1 && self->insideTableCell &&
      (isHeaderOrBlock(name) || strcmp(name, "hr") == 0 || matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)))) {
    // ponytail: rich cells use full-width flow; keep a grid only for plain inline content.
    if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
    self->fallbackTableRowToStacked();
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            {
              // Probe the dimensions from the entry's first bytes (early-aborted
              // inflate, a few KB) instead of extracting the whole image now —
              // extraction is deferred to the first render of the page (see
              // ImageBlock's lazy extractor). This is what keeps first-open of an
              // image-heavy chapter from stalling for seconds per image.
              ImageDimensions dims = {0, 0};
              ImageDimsProbe headerProbe;
              self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true);
              bool gotDimensions = headerProbe.getDimensions(dims);

              if (!gotDimensions) {
                // Retry with framebuffer scratch when the heap cannot fit the inflate window.
                GfxRenderer::FrameBufferLoan probeLoan(self->renderer);
                ImageDimsProbe retryProbe;
                self->epub->readItemContentsToStream(resolvedPath, retryProbe, 1024, /*allowEarlyStop=*/true);
                gotDimensions = retryProbe.getDimensions(dims);
              }

              if (!gotDimensions) {
                // No header within the stream (rare) — fall back to extracting the
                // whole image and probing the file. That can take seconds, so
                // surface the indexing popup first (single-shot per parser).
                if (self->popupFn && !self->imagePopupFired) {
                  self->imagePopupFired = true;
                  self->popupFn();
                }
                HalFile cachedImageFile;
                bool extractSuccess = false;
                if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                  {
                    // Same 32 KB inflate window as the probe; the popup is already up.
                    GfxRenderer::FrameBufferLoan extractLoan(self->renderer);
                    extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
                  }
                  cachedImageFile.flush();
                  cachedImageFile.close();
                }
                if (extractSuccess) {
                  // Retry to absorb SD-card sync latency on slow cards, and to close
                  // the silent-drop bug where a single getDimensions failure was fatal.
                  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                  for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                    if (attempt > 0) {
                      delay(50);  // Give a slow SD card time to finish syncing before retrying
                    }
                    gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                  }
                } else {
                  LOG_ERR("EHP", "Failed to extract image");
                }
              }

              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                const CssStyle& imgStyle = cssStyle;
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Apply vertical margins from the container to the image.
                // Top margin lives on the empty text block (deposited via vertical merge
                // in startNewTextBlock). Bottom margin was stripped by withoutBottom() for
                // deferred application at element close, so read it from the stack.
                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = bs.topInset();
                  if (self->blockStyleStack.size() > 1) {
                    imageMarginBottom = self->blockStyleStack.back().bottomInset();
                  }
                }

                self->applyPendingPageBreak();
                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  // A heading introducing the image moves with it when both fit on one page.
                  size_t carry = self->keepWithNextCarry();
                  if (carry > 0) {
                    const auto& elements = self->currentPage->elements;
                    const int carriedHeight = self->currentPageNextY - elements[elements.size() - carry]->yPos;
                    if (carriedHeight + imageMarginTop + displayHeight > self->viewportHeight) carry = 0;
                  }
                  self->breakPageCarryingLines(carry, 0, self->visibleTextOffset);
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                  self->currentPageVisibleOffsetSet = false;
                }

                // Apply top margin from container block. Clamp it so the image never
                // overflows the page bottom: a full-viewport-height image leaves no room
                // for the margin, and the break above only fires on non-empty pages, so a
                // fresh page would otherwise place the image at y=marginTop and run
                // marginTop pixels past viewportHeight. A large bottom reserve (status
                // bar / big screen margin) absorbs that overflow silently, but with a
                // thin reserve it crosses the physical screen edge and fails
                // ImageBlock::render's bounds check, dropping the image entirely.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int room = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(room > 0 ? room : 0);
                }
                self->currentPageNextY += imageMarginTop;

                auto imageBlock =
                    makeUniqueNoThrow<ImageBlock>(cachedImagePath, resolvedPath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = makeUniqueNoThrow<PageImage>(std::move(imageBlock), xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(std::move(pageImage));
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                self->noteContent(self->currentPageNextY, self->currentPageNextY + displayHeight);
                self->keepWithNextLines = 0;
                self->currentPageNextY += displayHeight + imageMarginBottom;

                // The image consumed the empty block's accumulated vertical spacing.
                // Reset the block so the Vertical merge in startNewTextBlock doesn't
                // re-apply the same margins to the next text paragraph.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                             ? CssTextAlign::Justify
                                             : static_cast<CssTextAlign>(self->paragraphAlignment);
                  self->currentTextBlock->setBlockStyle(resetStyle);
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->syntheticCharacterData = true;
        self->characterData(userData, alt.c_str(), alt.length());
        self->syntheticCharacterData = false;
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }

      // No alt text, skip
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (VisibleTextUtils::isNonVisibleElement(name)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Footnote indices are block-relative, so linked rows use ordinary flow.
      if (self->tableDepth >= 1 && self->insideTableCell && !self->tableRowStacked) {
        self->fallbackTableRowToStacked();
      }

      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkId = self->currentTextBlock ? self->currentTextBlock->addLinkTarget(href) : 0;
      self->currentFootnote.href[0] = '\0';
      if (self->currentFootnoteLinkId != 0) strcpy(self->currentFootnote.href, href);
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link.
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasTextDecoration = true;
      entry.textDecoration = CssTextDecoration::Underline;
      applyDirectionToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    self->applyBlockFontScale(headerBlockStyle, cssStyle, name);
    headerBlockStyle.keepWithNext = true;
    self->openBoxScope(headerBlockStyle, cssStyle);
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->pushBlockStyle(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->pushBlockTextStyleEntry(cssStyle);
    self->armFirstLetterDropCap(name, classAttr, idAttr);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->breakTextLine(self->blockStyleStack.back().withoutTop().withoutBottom());
    } else {
      self->currentCssStyle = cssStyle;
      auto blockStyle = userAlignmentBlockStyle;
      self->applyBlockFontScale(blockStyle, cssStyle, name);
      self->openBoxScope(blockStyle, cssStyle);
      const auto accumulated =
          self->blockStyleStack.back().getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Horizontal);
      self->pushBlockStyle(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      if (!self->currentTextBlock) {
        // OOM: layoutOom is latched; bail before the <li> marker path below
        // dereferences the missing block. parseStep() fails the build.
        return;
      }
      self->pushBlockTextStyleEntry(cssStyle);
      self->updateEffectiveInlineStyle();
      if (strcmp(name, "li") != 0) self->armFirstLetterDropCap(name, classAttr, idAttr);

      if (strcmp(name, "li") == 0) {
        // Innermost open <ul>/<ol> (if any) decides whether this item gets a bullet,
        // a number, or no marker at all (list-style-type: none). A malformed <li>
        // with no enclosing list falls back to the plain bullet.
        if (!self->listStack.empty() && self->listStack.back().styleNone) {
          // No marker: leave the block empty so it behaves like a normal paragraph.
        } else if (!self->listStack.empty() && self->listStack.back().ordered) {
          self->listStack.back().counter += 1;
          char marker[16];
          snprintf(marker, sizeof(marker), "%d.", self->listStack.back().counter);
          self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR, false, false, self->visibleTextOffset);
          self->listItemBulletOnly = true;
        } else {
          self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false,
                                          self->visibleTextOffset);
          self->listItemBulletOnly = true;
        }
      } else if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        // <ul>/<ol> container accumulation: pushing the block-style stack here includes
        // the container's own margin/padding in the inset children combine with, so a
        // hanging text-indent on <li><p> children keeps the first line on the page.
        ChapterHtmlSlimParser::ListContext ctx;
        ctx.ordered = strcmp(name, "ol") == 0;
        ctx.styleNone = cssStyle.hasListStyleType() && cssStyle.listStyleType == CssListStyleType::None;
        ctx.depth = self->depth;
        self->listStack.push_back(ctx);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::Underline, cssStyle);
  } else if (matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->pushDecorationStyleEntry(CssTextDecoration::LineThrough, cssStyle);
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyInlinePresentationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    applyTextDecorationToEntry(entry, cssStyle);
    applyInlinePresentationToEntry(entry, cssStyle);
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    if (cssStyle.hasFontSize() && self->tableDepth == 0 && self->partWordBufferIndex == 0 && self->currentTextBlock &&
        self->currentTextBlock->isEmpty() && (!self->inlineSize.valid || self->inlineSize.open)) {
      const float base =
          self->inlineSize.valid ? self->inlineSize.scale : self->currentTextBlock->getBlockStyle().fontScale;
      self->inlineSize.scale =
          std::clamp(cssStyle.fontSize.unit == CssUnit::Rem ? cssStyle.fontSize.value : base * cssStyle.fontSize.value,
                     0.5f, 3.0f);
      if (!self->inlineSize.valid) self->inlineSize.depth = self->depth;
      self->inlineSize.valid = true;
      self->inlineSize.open = true;
    }
    // A floated or oversized span opening a paragraph is a hand-made drop cap.
    if (self->tableDepth == 0 && self->dropCap.length == 0 && self->dropCap.spanDepth < 0 &&
        self->partWordBufferIndex == 0 && self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      const uint8_t lines = dropCapLines(cssStyle);
      if (lines > 0) {
        self->dropCap.spanDepth = self->depth;
        self->dropCap.lines = lines;
        self->dropCap.firstLetterPending = false;
        self->dropCap.bold =
            self->effectiveBold || (cssStyle.hasFontWeight() && cssStyle.fontWeight == CssFontWeight::Bold);
      }
    }
    // Handle span and other inline elements for CSS styling.
    const bool inheritedTableTextAlign = self->tableDepth >= 1 && cssStyle.hasTextAlign();
    // <small>/<big> default to the browser's relative sizes.
    const float tagScale = strcmp(name, "small") == 0 ? 0.83f : (strcmp(name, "big") == 0 ? 1.2f : 0.0f);
    const bool sized = cssStyle.hasFontSize() || tagScale > 0.0f;
    if (sized || cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasSmallCaps() || cssStyle.defined.whiteSpace || cssStyle.hasDirection() ||
        cssStyle.hasVerticalAlign() || inheritedTableTextAlign) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      applyTextDecorationToEntry(entry, cssStyle);
      applyInlinePresentationToEntry(entry, cssStyle);
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      if (inheritedTableTextAlign) {
        entry.hasTextAlign = true;
        entry.textAlign = cssStyle.textAlign;
      }
      applyVerticalAlignToEntry(entry, cssStyle);
      if (sized) {
        entry.hasFontScale = true;
        entry.fontScaleRem = cssStyle.hasFontSize() && cssStyle.fontSize.unit == CssUnit::Rem;
        entry.fontScale = cssStyle.hasFontSize() ? cssStyle.fontSize.value : tagScale;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  const bool countVisibleOffsets = self->insideBody && self->nonVisibleTextDepth == 0 && !self->syntheticCharacterData;
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  if (countVisibleOffsets) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Nested content needs an enclosing bounded cell collector.
  if (self->tableDepth > 1 && !self->insideTableCell) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing.
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  if (self->tableDepth == 1 && !self->insideTableCell) {
    bool onlyWhitespace = true;
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        onlyWhitespace = false;
        break;
      }
    }
    if (onlyWhitespace) {
      return;
    }
  }

  // Recreate flow storage for valid text (for example a caption) after a row.
  if (!self->currentTextBlock) {
    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->hyphenationEnabled, self->focusReadingEnabled,
                                                           flowStyle, self->paragraphIndentSpaces);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block for character data");
      return;
    }
    self->wordsExtractedInBlock = 0;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  if (self->inlineSize.valid && !self->inlineSize.open &&
      std::any_of(s, s + len, [](const char c) { return !isWhitespace(c); })) {
    self->inlineSize.valid = false;
  }

  uint32_t nextCodepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    const uint32_t codepointOffset = nextCodepointOffset;
    if (countVisibleOffsets && (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      nextCodepointOffset++;
    }

    // Initial-letter capture: a drop-cap span takes all of its text; ::first-letter takes
    // leading punctuation plus the block's first letter.
    const bool capturingSpan = self->dropCap.spanDepth >= 0;
    if (capturingSpan || (self->dropCap.firstLetterPending && !isWhitespace(s[i]) && self->partWordBufferIndex == 0 &&
                          !self->insideFootnoteLink && self->currentTextBlock->isEmpty())) {
      if (isWhitespace(s[i])) {
        // Several words: not an initial letter after all.
        if (self->dropCap.length > 0) self->cancelDropCapToWord();
      } else {
        const auto lead = static_cast<uint8_t>(s[i]);
        const int codepointLength = std::min(len - i, lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4)));
        if (self->captureDropCapCodepoint(s + i, codepointLength)) {
          i += codepointLength - 1;
          continue;
        }
      }
    }

    if (self->effectivePreserveWhitespace && isWhitespace(s[i])) {
      self->flushPartWordBuffer();
      self->currentTextBlock->suppressFirstLineIndent();
      if (s[i] == '\n') {
        const BlockStyle style = self->currentTextBlock->getBlockStyle().withoutTop().withoutBottom();
        self->breakTextLine(style);
        if (!self->currentTextBlock) return;
      } else if (s[i] != '\r') {
        // XML normalizes line endings; tabs use a bounded four-space expansion.
        const int count = s[i] == '\t' ? 4 : 1;
        for (int space = 0; space < count; ++space) {
          self->partWordBuffer[0] = ' ';
          self->partWordBufferIndex = 1;
          self->partWordVisibleOffset = codepointOffset;
          self->flushPartWordBuffer();
        }
      }
      continue;
    }

    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowVisibleOffset = self->partWordVisibleOffset;
        const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const unsigned char* const safeEnd = offsetPtr + safeLen;
        while (offsetPtr < safeEnd) {
          utf8NextCodepoint(&offsetPtr);
          overflowVisibleOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowVisibleOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // Block creation failed (OOM): nothing to soft-flush.
  if (!self->currentTextBlock) {
    return;
  }

  // Keep token growth bounded: CSS-heavy spans can fragment text into many tiny
  // words, so flush earlier when embedded CSS is active. We still keep the
  // "exclude last line" behavior to preserve paragraph flow across chunks.
  const size_t blockWordCount = self->currentTextBlock->size();
  const size_t softFlushThreshold =
      self->embeddedStyle ? TEXT_BLOCK_SOFT_FLUSH_WORDS_WITH_CSS : TEXT_BLOCK_SOFT_FLUSH_WORDS;
  if (blockWordCount > softFlushThreshold && !self->inRuby) {
    LOG_DBG("EHP", "Text block soft flush (%u words)", static_cast<unsigned>(blockWordCount));
    self->makePages(/*includeLastLine=*/false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->nonVisibleTextDepth > 0) {
    self->nonVisibleTextDepth--;
  }

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool insideSkippedSubtree = self->depth - 1 >= self->skipUntilDepth;

  if (!insideSkippedSubtree && self->tableDepth > 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->nextWordContinues = false;
    self->tableDepth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table flattened into enclosing cell");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, LINETHROUGH_TAGS, std::size(LINETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  // A captured letter without body text is ordinary block content.
  if (headerOrBlockTag && strcmp(name, "br") != 0 && !insideSkippedSubtree && self->currentTextBlock &&
      self->currentTextBlock->isEmpty() && self->dropCap.length > 0) {
    self->cancelDropCapToWord();
    self->flushPartWordBuffer();
  }

  self->depth -= 1;

  // The captured drop-cap letter waits for its block's layout.
  if (self->dropCap.spanDepth == self->depth) {
    self->dropCap.spanDepth = -1;
  }
  if (self->inlineSize.valid && self->inlineSize.depth == self->depth) {
    self->inlineSize.open = false;
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
    self->currentFootnoteLinkId = 0;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->closeTableCell();
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->finishTableRow();
    self->nextWordContinues = false;
  }

  if (!insideSkippedSubtree && self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->finishTableRow();
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTextBlock.reset();
    self->tableDepth = 0;
    self->insideTableCell = false;
    self->tableRowStacked = false;
    self->tableRowsSpannedRemaining = 0;
    self->tableRowTextBytes = 0;
    self->tableRowCells.clear();
    self->nextWordContinues = false;

    const BlockStyle flowStyle =
        self->blockStyleStack.empty() ? BlockStyle() : self->blockStyleStack.back().withoutBottom();
    self->currentTextBlock = makeUniqueNoThrow<ParsedText>(self->hyphenationEnabled, self->focusReadingEnabled,
                                                           flowStyle, self->paragraphIndentSpaces);
    if (!self->currentTextBlock) {
      LOG_ERR("EHP", "OOM: text block after table");
    }
    self->wordsExtractedInBlock = 0;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag && !insideSkippedSubtree) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      const bool breakAfter = self->blockStyleStack.back().pageBreakAfter;
      self->blockStyleStack.pop_back();
      // Start a new text block with the parent style to prevent subsequent bare text
      // from inheriting the closed block style (e.g. alignment or margins).
      // Vertical margins and paddings are stripped
      self->startNewTextBlock(self->blockStyleStack.back().withoutTop().withoutBottom());
      self->updateEffectiveInlineStyle();
      if (!self->boxScopes.empty() && self->boxScopes.back().depth == self->depth) self->closeBoxScope();
      // Set after the closing element's own text was laid out above.
      if (breakAfter) self->pendingPageBreak = true;
    }

    // </li> closes: if the bullet never got inline text (empty <li> or <li> with only
    // block children that were flushed), clear the flag so the next sibling doesn't
    // merge into this block.
    if (strcmp(name, "li") == 0) {
      self->listItemBulletOnly = false;
    }
  }

  // </ul> or </ol> closes: pop its list context so a following sibling list at the
  // same nesting level starts its own counter/style instead of inheriting this one's.
  // Guarded by depth (not just tag name + non-empty stack): a display:none <ul>/<ol>
  // returns early in startElement without ever pushing a context (see the
  // hasDisplay()/CssDisplay::None branch above), so its closing tag must not pop
  // the *parent* list's context out from under still-unprocessed siblings.
  if ((strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) && !self->listStack.empty() &&
      self->listStack.back().depth == self->depth) {
    self->listStack.pop_back();
  }
  if (strcmp(name, "body") == 0) {
    self->insideBody = false;
  }
  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::beginParse() {
  htmlEnded_ = false;
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);
  boxScopes.clear();
  boxScopes.reserve(MAX_BOX_SCOPES);
  dropCap = DropCapState{};

  listStack.clear();
  listStack.reserve(4);
  tableDepth = 0;
  insideTableCell = false;
  tableRowStacked = false;
  tableRowsSpannedRemaining = 0;
  tableRowTextBytes = 0;
  tableRowCells.clear();
  for (auto& lines : tableCellLines) {
    lines.clear();
  }
  tableLineVisibleOffsets.clear();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFile_.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  parseStartTime_ = millis();
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  // Layout OOM latched during the previous buffer's callbacks: fail the build
  // instead of emitting pages with silently missing text.
  if (layoutOom || (currentTextBlock && currentTextBlock->hadDroppedWords())) {
    LOG_ERR("EHP", "Text layout dropped content (OOM); failing section build");
    return ParseStatus::Error;
  }

  void* const buf = XML_GetBuffer(xmlParser_, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);

  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const int done = parseFile_.available() == 0;

  if (XML_ParseBuffer(xmlParser_, static_cast<int>(len), done) == XML_STATUS_ERROR) {
    if (htmlEnded_) {
      LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      return ParseStatus::Done;
    }
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(xmlParser_),
            XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    return ParseStatus::Error;
  }

  return done ? ParseStatus::Done : ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (xmlParser_) {
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  // Only close the file if it was successfully opened in beginParse()
  if (parseFile_.isOpen()) {
    parseFile_.close();
  }
}

bool ChapterHtmlSlimParser::finishParse() {
  // Same check as parseStep(): drops in the final buffer would otherwise slip
  // through because Done is returned before the next step's check runs.
  if (layoutOom || (currentTextBlock && currentTextBlock->hadDroppedWords())) {
    LOG_ERR("EHP", "Text layout dropped content (OOM); failing section build");
    return false;
  }

  if (xmlParser_) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - parseStartTime_);
    destroyXmlParser(xmlParser_);
    xmlParser_ = nullptr;
  }
  parseFile_.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    // Re-check: makePages() latches layoutOom for lines dropped DURING this
    // final layout, which the entry check above cannot have seen.
    if (layoutOom) {
      LOG_ERR("EHP", "Text layout dropped content (OOM); failing section build");
      return false;
    }
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    setCurrentPageVisibleOffset(visibleTextOffset);
    emitCurrentPage();
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::applyBlockFontScale(BlockStyle& blockStyle, const CssStyle& cssStyle,
                                                const char* tagName) const {
  const float parentScale = blockStyleStack.empty() ? 1.0f : blockStyleStack.back().fontScale;
  float scale = parentScale;
  if (cssStyle.hasFontSize()) {
    scale = cssStyle.fontSize.unit == CssUnit::Rem ? cssStyle.fontSize.value : parentScale * cssStyle.fontSize.value;
  } else if (tagName[0] == 'h' && tagName[1] >= '1' && tagName[1] <= '6' && tagName[2] == '\0') {
    // Browser default heading sizes, h1..h6.
    static constexpr float HEADING_SCALES[] = {2.0f, 1.5f, 1.17f, 1.0f, 0.83f, 0.67f};
    scale = parentScale * HEADING_SCALES[tagName[1] - '1'];
  }
  blockStyle.fontScale = std::clamp(scale, 0.5f, 3.0f);
  blockStyle.fontScaleDefined = true;
}

namespace {
// Built-in reader families live in flash at these sizes, so switching a block to
// another size costs no RAM. SD and vector fonts get their sizes from the renderer's
// variant provider.
constexpr uint8_t BUILTIN_LADDER_POINTS[] = {12, 14, 16, 18};
constexpr int BUILTIN_LADDERS[][std::size(BUILTIN_LADDER_POINTS)] = {
    {NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_18_FONT_ID},
    {NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_18_FONT_ID},
};
}  // namespace

int ChapterHtmlSlimParser::fontIdForScale(const float scale) const {
  if (std::fabs(scale - 1.0f) < 0.05f) return fontId;
  for (const auto& ladder : BUILTIN_LADDERS) {
    const int* base = std::find(std::begin(ladder), std::end(ladder), fontId);
    if (base == std::end(ladder)) continue;
    const float target = BUILTIN_LADDER_POINTS[base - ladder] * scale;
    size_t pick = scale < 1.0f ? 0 : static_cast<size_t>(base - ladder);
    for (size_t i = 0; i < std::size(BUILTIN_LADDER_POINTS); ++i) {
      const float points = BUILTIN_LADDER_POINTS[i];
      // Shrinking takes the largest size at or below the target, so small print
      // never rounds back up to body size; growing takes the nearest size.
      if (scale < 1.0f ? points <= target
                       : std::fabs(points - target) < std::fabs(BUILTIN_LADDER_POINTS[pick] - target)) {
        pick = i;
      }
    }
    return renderer.getFontMap().count(ladder[pick]) ? ladder[pick] : fontId;
  }
  const int variant = renderer.resolveFontVariant(fontId, scale);
  return variant != 0 ? variant : fontId;
}

int ChapterHtmlSlimParser::prepareBlockFont() {
  BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int layoutFontId = fontIdForScale(inlineSize.valid ? inlineSize.scale : blockStyle.fontScale);
  for (uint8_t slot = 1; slot < currentTextBlock->sizeSlotsInUse(); ++slot) {
    currentTextBlock->setSizeSlotFontId(slot, fontIdForScale(currentTextBlock->sizeSlotScale(slot)));
  }
  blockStyle.fontId = layoutFontId == fontId ? 0 : layoutFontId;
  return layoutFontId;
}

size_t ChapterHtmlSlimParser::linesToCarry(size_t& paragraphLines) const {
  paragraphLines = 0;
  if (layoutParagraph && !layoutParagraphHasDropCap) {
    if (paragraphLinesOnPage == 1) {
      paragraphLines = 1;  // the paragraph's first line would sit alone at the page bottom
    } else if (paragraphLinesOnPage >= 2 && layoutParagraph->linesAfterCurrentLine() == 0) {
      // The last line would open the next page alone; taking one line along must not
      // leave an orphan behind.
      paragraphLines = paragraphLinesOnPage >= 3 ? 1 : 2;
    }
  }
  // A heading moves too when none of what follows it would stay on this page.
  size_t carry = paragraphLines;
  if (keepWithNextLines > 0 && paragraphLines == paragraphLinesOnPage) carry += keepWithNextLines;
  if (canCarry(carry)) return carry;
  if (canCarry(paragraphLines)) return paragraphLines;
  paragraphLines = 0;
  return 0;
}

size_t ChapterHtmlSlimParser::keepWithNextCarry() const { return canCarry(keepWithNextLines) ? keepWithNextLines : 0; }

// Only the page's trailing text lines move, and never everything on the page.
bool ChapterHtmlSlimParser::canCarry(const size_t carry) const {
  if (carry == 0) return true;
  if (!currentPage || carry > recentLineCount || carry > MAX_CARRIED_LINES) return false;
  const auto& elements = currentPage->elements;
  if (elements.size() <= carry) return false;
  for (size_t i = 0; i < carry; ++i) {
    if (elements[elements.size() - 1 - i]->getTag() != TAG_PageLine) return false;
  }
  return true;
}

// Completes the page, moving its last `carry` lines (validated by canCarry) and their
// link areas to the top of the next page with their spacing intact.
void ChapterHtmlSlimParser::breakPageCarryingLines(const size_t carry, const size_t paragraphLines,
                                                   const uint32_t visibleOffset) {
  std::unique_ptr<PageElement> carried[MAX_CARRIED_LINES];
  uint32_t carriedOffsets[MAX_CARRIED_LINES] = {};
  std::vector<PageLink> carriedLinks;
  int16_t carriedTop = 0;
  const int16_t carriedBottom = currentPageNextY;
  for (size_t i = 0; i < carry; ++i) {
    carried[carry - 1 - i] = std::move(currentPage->elements.back());
    currentPage->elements.pop_back();
    carriedOffsets[carry - 1 - i] = recentLineOffsets[recentLineCount - 1 - i];
  }
  if (carry > 0) {
    carriedTop = carried[0]->yPos;
    auto& links = currentPage->links;
    for (auto it = links.begin(); it != links.end();) {
      if (it->y + it->height > carriedTop) {
        if (carriedLinks.empty()) carriedLinks.reserve(links.size());
        carriedLinks.push_back(*it);
        it = links.erase(it);
      } else {
        ++it;
      }
    }
  }

  setCurrentPageVisibleOffset(visibleOffset);
  emitCurrentPage();
  completedPageCount++;
  currentPage.reset(new Page());
  currentPageNextY = 0;
  currentPageVisibleOffsetSet = false;

  for (size_t i = 0; i < carry; ++i) {
    carried[i]->yPos = static_cast<int16_t>(carried[i]->yPos - carriedTop);
    setCurrentPageVisibleOffset(carriedOffsets[i]);
    currentPage->elements.push_back(std::move(carried[i]));
    recentLineOffsets[recentLineCount++] = carriedOffsets[i];
  }
  if (carry > 0) {
    currentPageNextY = static_cast<int16_t>(carriedBottom - carriedTop);
    noteContent(0, currentPageNextY);
  }
  for (const PageLink& link : carriedLinks) {
    currentPage->addLink(link.href, link.x, static_cast<int16_t>(link.y - carriedTop), link.width, link.height);
  }
  paragraphLinesOnPage = static_cast<uint8_t>(paragraphLines);
}

void ChapterHtmlSlimParser::addLineToPage(std::unique_ptr<TextBlock> line, const uint32_t visibleOffset) {
  const int lineFontId = line->blockFontId(fontId);
  const int rubyShift = line->getRubyShift(renderer.getFontAscenderSize(lineFontId));
  // Taller inline words push the shared baseline down.
  const int baseLineHeight = renderer.getLineHeight(lineFontId, lineCompression) + line->extraAscent(renderer, fontId);
  const int lineHeight = baseLineHeight + rubyShift;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    size_t paragraphLines = 0;
    const size_t carry = linesToCarry(paragraphLines);
    breakPageCarryingLines(carry, paragraphLines, visibleOffset);
  }
  setCurrentPageVisibleOffset(visibleOffset);

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  for (const auto& link : line->takeLinkSpans()) {
    if (!currentPage->addLink(link.href, static_cast<int16_t>(xOffset + link.x),
                              static_cast<int16_t>(currentPageNextY + rubyShift - link.topLift), link.width,
                              static_cast<int16_t>(baseLineHeight + link.topLift))) {
      LOG_DBG("EHP", "Dropped page link: %.48s", link.href);
    }
  }
  auto pageLine = makeUniqueNoThrow<PageLine>(std::move(line), xOffset, currentPageNextY);
  if (!pageLine) {
    LOG_ERR("EHP", "OOM: PageLine");
    return;
  }
  currentPage->elements.push_back(std::move(pageLine));
  noteContent(currentPageNextY, currentPageNextY + lineHeight);
  currentPageNextY += lineHeight;
  if (layoutParagraph && paragraphLinesOnPage < UINT8_MAX) paragraphLinesOnPage++;
  if (recentLineCount == MAX_CARRIED_LINES) {
    memmove(recentLineOffsets, recentLineOffsets + 1, sizeof(recentLineOffsets[0]) * (MAX_CARRIED_LINES - 1));
    recentLineCount--;
  }
  recentLineOffsets[recentLineCount++] = visibleOffset;
}

void ChapterHtmlSlimParser::makePages(const bool includeLastLine, const bool paragraphEnd) {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  // Latch before layout: startNewTextBlock() replaces the block right after
  // this returns, which would otherwise lose its dropped-words flag before
  // parseStep()/finishParse() get to check it.
  if (currentTextBlock->hadDroppedWords()) {
    layoutOom = true;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    currentPageVisibleOffsetSet = false;
  }
  applyPendingPageBreak();

  const int lineHeight = renderer.getLineHeight(fontId, lineCompression);

  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();

  layoutCurrentBlock(includeLastLine);
  if (includeLastLine) {
    // A heading's lines on this page wait to move with what follows it.
    keepWithNextLines =
        blockStyle.keepWithNext
            ? static_cast<uint8_t>(std::min<size_t>(MAX_CARRIED_LINES, keepWithNextLines + paragraphLinesOnPage))
            : 0;
    paragraphLinesOnPage = 0;
    layoutParagraphHasDropCap = false;
  }

  // Latch again after layout: extractLine can drop a whole line (TextBlock
  // arena OOM) during the call above, after the pre-layout latch ran, and the
  // block is replaced right after this returns.
  if (currentTextBlock->hadDroppedWords()) {
    layoutOom = true;
  }

  // Trailing spacing and footnotes only apply when the block is finalized
  if (includeLastLine) {
    // Fallback: transfer any remaining pending footnotes to current page.
    // Normally addLineToPage handles this via word-index tracking, but this catches
    // edge cases where a footnote's word index equals the exact block size.
    if (!pendingFootnotes.empty() && currentPage) {
      for (const auto& [idx, fn] : pendingFootnotes) {
        currentPage->addFootnote(fn.number, fn.href);
      }
      pendingFootnotes.clear();
    }

    if (!paragraphEnd) return;

    // Apply bottom spacing after the paragraph (stored in pixels)
    if (blockStyle.marginBottom > 0) {
      currentPageNextY += blockStyle.marginBottom;
    }
    if (blockStyle.paddingBottom > 0) {
      currentPageNextY += blockStyle.paddingBottom;
    }

    // Extra paragraph spacing if enabled (default behavior)
    if (extraParagraphSpacing) {
      currentPageNextY += lineHeight / 2;
    }
  }
}
