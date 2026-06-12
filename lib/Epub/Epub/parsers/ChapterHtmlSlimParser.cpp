#include "ChapterHtmlSlimParser.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/blocks/TableBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices (~380KB heap). TOC anchors bypass this cap.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* IMAGE_TAGS[] = {"img"};
constexpr const char* SKIP_TAGS[] = {"head"};

static int16_t parseHtmlWidthAttr(const char* s, int16_t viewportWidth, float emSize) {
  if (!s || !*s) return 0;
  char* end;
  const float val = strtof(s, &end);
  if (end == s) return 0;  // no numeric part
  while (*end == ' ') ++end;
  if (*end == '%') {
    return static_cast<int16_t>(val * viewportWidth / 100.0f);
  }
  if (strncmp(end, "em", 2) == 0) {
    return static_cast<int16_t>(val * emSize);
  }
  if (strncmp(end, "rem", 3) == 0) {
    return static_cast<int16_t>(val * emSize);
  }
  if (strncmp(end, "pt", 2) == 0) {
    return static_cast<int16_t>(val * 1.33f);
  }
  return static_cast<int16_t>(val);  // treat bare number or px as pixels
}

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
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

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;
  effectiveDirectionDefined = currentCssStyle.hasDirection();
  effectiveDirection = currentCssStyle.direction;
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
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
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

  // Keep inherited direction in the active empty text block so upcoming block starts
  // can inherit from non-block ancestors such as <html dir="rtl"> / <body dir="rtl">.
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    if (effectiveDirectionDefined) {
      style.directionDefined = true;
      style.isRtl = (effectiveDirection == CssTextDirection::Rtl);
    } else {
      style.directionDefined = false;
      style.isRtl = false;
    }
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (inTableCellMode) {
    // Inside a table cell: merge all content into the single cell ParsedText without resetting.
    flushPendingAnchor();
    return;
  }
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // The stack accumulates horizontal margins and text properties from ancestors.
      // Vertical margins are per-element and not inherited through the stack, but
      // container elements deposit their vertical margins on the empty block when they
      // open. Merge those into the new style so the first child in a container inherits
      // the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));

      flushPendingAnchor();
      return;
    }

    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, blockStyle));
  wordsExtractedInBlock = 0;
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

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
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
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

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

  // Extract class, style, id, and dir attributes for CSS/RTL processing
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
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
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
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
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
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

  if (strcmp(name, "table") == 0) {
    // Nested tables are not supported — skip their content
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->tableDepth += 1;
    self->tableAccumRows.clear();
    self->tableAccumRows.reserve(8);  // conservative estimate; avoids reallocation for typical tables
    self->tableTotalCols = 0;
    self->tableRowspanTracker.clear();
    self->tableTruncated = false;
    self->tableHasFlushedRows = false;
    // Border declared on the <table> element itself.
    // Handles e.g. <table style="border: 1px solid"> or a CSS rule like
    // "table { border-style: solid }" that targets the table directly.
    self->tableDrawBorderTop = cssStyle.isBorderTopVisible();
    self->tableDrawBorderBottom = cssStyle.isBorderBottomVisible();
    self->tableDrawBorderLeft = cssStyle.isBorderLeftVisible();
    self->tableDrawBorderRight = cssStyle.isBorderRightVisible();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    // OOM heap guard: stop accumulating rows if free heap is critically low.
    // The rows accumulated so far will still be emitted at </table>.
    if (!self->tableTruncated && ESP.getFreeHeap() < 60000) {
      LOG_ERR("EHP", "Table truncated: low heap (%u bytes free)", ESP.getFreeHeap());
      self->tableTruncated = true;
    }
    if (self->tableTruncated) {
      self->depth += 1;
      return;
    }
    self->tableAccumRows.emplace_back();
    auto& row = self->tableAccumRows.back();
    row.drawBorderTop |= cssStyle.isBorderTopVisible();
    row.drawBorderBottom |= cssStyle.isBorderBottomVisible();
    self->tableDrawInnerColDividers |= (cssStyle.isBorderLeftVisible() || cssStyle.isBorderRightVisible());
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->tableTruncated) {
      self->depth += 1;
      return;
    }
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }

    // Guard against malformed HTML where <td> appears without a prior <tr>
    if (self->tableAccumRows.empty()) {
      self->tableAccumRows.emplace_back();
    }

    const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    const float vw = static_cast<float>(self->viewportWidth);

    const int16_t defPadX = TableBlock::CELL_PADDING_X;
    const int16_t defPadY = TableBlock::CELL_PADDING_Y;
    const int16_t padLeft = cssStyle.hasPaddingLeft() ? cssStyle.paddingLeft.toPixelsInt16(emSize, vw) : defPadX;
    const int16_t padRight = cssStyle.hasPaddingRight() ? cssStyle.paddingRight.toPixelsInt16(emSize, vw) : defPadX;
    const int16_t padTop = cssStyle.hasPaddingTop() ? cssStyle.paddingTop.toPixelsInt16(emSize, vw) : defPadY;
    const int16_t padBottom = cssStyle.hasPaddingBottom() ? cssStyle.paddingBottom.toPixelsInt16(emSize, vw) : defPadY;

    int16_t reqWidth = 0;
    uint8_t cellColspan = 1;
    uint8_t cellRowspan = 1;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "width") == 0) {
          reqWidth = parseHtmlWidthAttr(atts[i + 1], static_cast<int16_t>(self->viewportWidth), emSize);
        } else if (strcmp(atts[i], "colspan") == 0) {
          const int cs = atoi(atts[i + 1]);
          if (cs > 1 && cs <= TableBlock::MAX_COLS) cellColspan = static_cast<uint8_t>(cs);
        } else if (strcmp(atts[i], "rowspan") == 0) {
          const int rs = atoi(atts[i + 1]);
          if (rs == 0)
            cellRowspan = TableBlock::MAX_COLS;  // HTML rowspan="0" = span to end of table group
          else if (rs > 1 && rs <= TableBlock::MAX_COLS)
            // Reject rowspan values outside the range of [1, MAX_COLS] to
            // prevent the phantom-cell loop in layout from running out of memory or looping excessively.
            cellRowspan = static_cast<uint8_t>(rs);
        }
      }
    }
    if (reqWidth == 0 && cssStyle.hasImageWidth()) {
      reqWidth = cssStyle.imageWidth.toPixelsInt16(emSize, vw);
    }

    CssTextAlign cellAlign;
    if (cssStyle.hasTextAlign()) {
      cellAlign = cssStyle.textAlign;
    } else if (self->paragraphAlignment != static_cast<uint8_t>(CssTextAlign::None)) {
      cellAlign = static_cast<CssTextAlign>(self->paragraphAlignment);
    } else {
      cellAlign = CssTextAlign::Justify;
    }

    BlockStyle cellBlockStyle;
    cellBlockStyle.textAlignDefined = true;
    cellBlockStyle.alignment = cellAlign;

    self->tableAccumRows.back().cells.emplace_back();
    auto& newCell = self->tableAccumRows.back().cells.back();
    newCell.paddingLeft = padLeft;
    newCell.paddingRight = padRight;
    newCell.paddingTop = padTop;
    newCell.paddingBottom = padBottom;
    newCell.requestedWidth = reqWidth;
    newCell.colspan = cellColspan;
    newCell.rowspan = cellRowspan;

    auto cellText = makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled,
                                                  self->focusReadingEnabled, cellBlockStyle);
    if (!cellText) {
      LOG_ERR("EHP", "OOM: ParsedText");
      self->tableAccumRows.back().cells.pop_back();  // undo emplace_back to keep state consistent
      return;
    }
    self->currentTextBlock = std::move(cellText);
    self->inTableCellMode = true;

    if (cssStyle.hasFontWeight()) {
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasBold = true;
      entry.bold = (cssStyle.fontWeight == CssFontWeight::Bold);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    } else if (strcmp(name, "th") == 0) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
      self->updateEffectiveInlineStyle();
    }
    if (cssStyle.hasFontStyle()) {
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasItalic = true;
      entry.italic = (cssStyle.fontStyle == CssFontStyle::Italic);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }

    // Border declared on individual <td>/<th> cells rather than on <table>.
    // Some EPUBs use border-collapse:collapse on the table
    // with no border-style on the table element; the actual borders live on each cell.
    // Top/bottom accumulate per-row (finalized in </tr>); left/right remain table-wide.
    self->tableAccumRows.back().drawBorderTop |= cssStyle.isBorderTopVisible();
    self->tableAccumRows.back().drawBorderBottom |= cssStyle.isBorderBottomVisible();
    self->tableDrawBorderLeft |= cssStyle.isBorderLeftVisible();
    self->tableDrawBorderRight |= cssStyle.isBorderRightVisible();
    // Inner column dividers are cell-driven (not table-element-driven), so track separately.
    self->tableDrawInnerColDividers |= (cssStyle.isBorderLeftVisible() || cssStyle.isBorderRightVisible());

    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->cssParser->resolveStyle("img", classAttr);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
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

            // Extract image to cache file
            HalFile cachedImageFile;
            bool extractSuccess = false;
            if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
              extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
              cachedImageFile.flush();
              cachedImageFile.close();
              delay(50);  // Give SD card time to sync
            }

            if (extractSuccess) {
              // Get image dimensions
              ImageDimensions dims = {0, 0};
              ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
              if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle = self->cssParser ? self->cssParser->resolveStyle("img", classAttr) : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty()) {
                  imgStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
                }
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

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                       self->xpathListItemIndex);
                  self->completedPageCount++;
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                // Apply top margin from container block
                self->currentPageNextY += imageMarginTop;

                // Create ImageBlock and add to page
                auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
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
            } else {
              LOG_ERR("EHP", "Failed to extract image");
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
        self->characterData(userData, alt.c_str(), alt.length());
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

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
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
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      applyDirectionToEntry(entry, cssStyle);
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
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->blockStyleStack.back().withoutBottom());
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
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
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
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
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
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
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasDirection() || cssStyle.hasVerticalAlign()) {
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
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      applyDirectionToEntry(entry, cssStyle);
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        }
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

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
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

  for (int i = 0; i < len; i++) {
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
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line.
  // Skip this optimisation inside table cells — cells are laid out in bulk at </tr>.
  if (!self->inTableCellMode && self->currentTextBlock->size() > 750) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, effectiveWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
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

void ChapterHtmlSlimParser::recomputeRowMetrics(TableRowAccum& row) {
  uint8_t maxLines = 0;
  int16_t maxPadTop = TableBlock::CELL_PADDING_Y;
  int16_t maxPadBot = TableBlock::CELL_PADDING_Y;
  for (const auto& ca : row.cells) {
    const auto lc = static_cast<uint8_t>(std::min(ca.lines.size(), size_t(255)));
    if (lc > maxLines) maxLines = lc;
    if (ca.paddingTop > maxPadTop) maxPadTop = ca.paddingTop;
    if (ca.paddingBottom > maxPadBot) maxPadBot = ca.paddingBottom;
  }
  row.maxLines = maxLines;
  row.maxPaddingTop = maxPadTop;
  row.maxPaddingBottom = maxPadBot;
}

void ChapterHtmlSlimParser::emitTablePageSlices(ChapterHtmlSlimParser* self, bool applyTopBorder,
                                                bool applyBottomBorder, bool isLastFlush) {
  if (self->tableAccumRows.empty() || self->tableTotalCols == 0) return;

  const auto numCols = static_cast<uint8_t>(self->tableTotalCols);
  const bool hasPerColWidths = (static_cast<int>(self->tableColWidths.size()) == numCols);
  const auto colWidthFallback = static_cast<int16_t>(numCols > 0 ? self->viewportWidth / numCols : self->viewportWidth);
  const auto lh = static_cast<int16_t>(self->renderer.getLineHeight(self->fontId) * self->lineCompression);

  size_t rowStart = 0;
  size_t totalRows = self->tableAccumRows.size();  // non-const: may grow if a row is split

  while (rowStart < totalRows) {
    // For incremental (non-final) flushes: if all remaining rows fit on the current page,
    // stop here — leave them in the accumulator so future rows can join them.
    if (!isLastFlush) {
      int32_t remainH = 1;  // top border
      for (size_t r = rowStart; r < totalRows; r++) {
        const auto& ra = self->tableAccumRows[r];
        remainH += 1 + ra.maxLines * lh + ra.maxPaddingTop + ra.maxPaddingBottom;
      }
      if (remainH <= self->viewportHeight - self->currentPageNextY) break;
    }

    // Apply outer-table border to the correct rows — deferred until after the early-exit
    // check so accumulator state is not mutated when nothing is emitted.
    if (rowStart == 0 && applyTopBorder) {
      self->tableAccumRows.front().drawBorderTop |= self->tableDrawBorderTop;
    }
    if (rowStart == 0 && applyBottomBorder) {
      self->tableAccumRows.back().drawBorderBottom |= self->tableDrawBorderBottom;
    }

    if (!self->currentPage) {
      self->currentPage = makeUniqueNoThrow<Page>();
      if (!self->currentPage) {
        LOG_ERR("EHP", "OOM: Page");
        return;
      }
      self->currentPageNextY = 0;
    }

    if (self->currentPageNextY >= self->viewportHeight) {
      self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex, self->xpathListItemIndex);
      self->completedPageCount++;
      self->currentPage = makeUniqueNoThrow<Page>();
      if (!self->currentPage) {
        LOG_ERR("EHP", "OOM: Page");
        return;
      }
      self->currentPageNextY = 0;
    }

    const int16_t available = static_cast<int16_t>(self->viewportHeight - self->currentPageNextY);

    // Greedily fit as many rows as possible into `available` pixels.
    // The first row is always included even if it doesn't fit (avoids infinite loop).
    // Height accounting matches TableBlock::totalHeight():
    //   (numRows + 1) border px  +  sum(maxPaddingTop + maxLines * lh + maxPaddingBottom)
    int16_t accumulated = 1;  // top border of the slice
    size_t rowEnd = rowStart;
    while (rowEnd < totalRows) {
      const auto& ra = self->tableAccumRows[rowEnd];
      const int16_t rowCost = static_cast<int16_t>(1 + ra.maxLines * lh + ra.maxPaddingTop + ra.maxPaddingBottom);
      // Only break if at least one row has already been included.
      if (rowEnd > rowStart && accumulated + rowCost > available) {
        break;
      }
      accumulated += rowCost;
      rowEnd++;
    }

    // If the sole row overflows on a non-empty page, flush first so that the
    // row-splitting code below runs on a fresh page where available == viewportHeight.
    if (rowEnd == rowStart + 1 && accumulated > available && self->currentPageNextY > 0) {
      self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex, self->xpathListItemIndex);
      self->completedPageCount++;
      self->currentPage = makeUniqueNoThrow<Page>();
      if (!self->currentPage) {
        LOG_ERR("EHP", "OOM: Page");
        return;
      }
      self->currentPageNextY = 0;
      continue;  // re-evaluate available on the new page
    }

    // If the sole row to emit is taller than the full page, split it line by line
    // so the overflow continues on the next page rather than rendering off-screen.
    if (rowEnd == rowStart + 1 && accumulated > available) {
      auto& accumRow = self->tableAccumRows[rowStart];
      const int16_t contentAvail =
          static_cast<int16_t>(available - 2 - accumRow.maxPaddingTop - accumRow.maxPaddingBottom);
      const int16_t fittableLines = static_cast<int16_t>(std::max(int16_t(1), static_cast<int16_t>(contentAvail / lh)));

      if (fittableLines < static_cast<int16_t>(accumRow.maxLines)) {
        TableRowAccum remainder;
        remainder.maxPaddingTop = accumRow.maxPaddingTop;
        remainder.maxPaddingBottom = accumRow.maxPaddingBottom;
        uint8_t remMaxLines = 0;

        for (auto& ca : accumRow.cells) {
          TableCellAccum remCell;
          ca.copySpanAndPaddingTo(remCell);

          const auto splitAt = static_cast<size_t>(fittableLines);
          if (ca.lines.size() > splitAt) {
            remCell.lines.assign(std::make_move_iterator(ca.lines.begin() + static_cast<ptrdiff_t>(splitAt)),
                                 std::make_move_iterator(ca.lines.end()));
            ca.lines.resize(splitAt);
          }
          const auto remCount = static_cast<uint8_t>(std::min(remCell.lines.size(), size_t(255)));
          if (remCount > remMaxLines) remMaxLines = remCount;
          remainder.cells.push_back(std::move(remCell));
        }
        remainder.maxLines = remMaxLines;
        accumRow.maxLines = static_cast<uint8_t>(fittableLines);

        // Insert remainder immediately after the current row.
        self->tableAccumRows.insert(self->tableAccumRows.begin() + static_cast<ptrdiff_t>(rowStart + 1),
                                    std::move(remainder));
        totalRows++;  // one more row to process
        // rowEnd stays at rowStart+1; fall through to emit the trimmed row.
      }
    }

    auto tableBlock = makeUniqueNoThrow<TableBlock>();
    if (!tableBlock) {
      LOG_ERR("EHP", "OOM: TableBlock");
      return;
    }
    tableBlock->numCols = numCols;
    tableBlock->colWidth = colWidthFallback;
    tableBlock->lineHeight = lh;
    tableBlock->drawBorderLeft = self->tableDrawBorderLeft;
    tableBlock->drawBorderRight = self->tableDrawBorderRight;
    tableBlock->drawInnerColDividers = self->tableDrawInnerColDividers;
    if (hasPerColWidths) tableBlock->colWidths = self->tableColWidths;
    tableBlock->rows.reserve(rowEnd - rowStart);
    for (size_t r = rowStart; r < rowEnd; r++) {
      TableRow row;
      row.maxLines = self->tableAccumRows[r].maxLines;
      row.maxPaddingTop = self->tableAccumRows[r].maxPaddingTop;
      row.maxPaddingBottom = self->tableAccumRows[r].maxPaddingBottom;
      row.drawBorderTop = self->tableAccumRows[r].drawBorderTop;
      row.drawBorderBottom = self->tableAccumRows[r].drawBorderBottom;
      row.cells.reserve(self->tableAccumRows[r].cells.size());
      for (auto& ca : self->tableAccumRows[r].cells) {
        TableCell cell = ca.toTableCell();
        cell.lines = std::move(ca.lines);
        row.cells.push_back(std::move(cell));
      }
      tableBlock->rows.push_back(std::move(row));
    }

    const int16_t tableHeight = tableBlock->totalHeight();
    auto* ptRaw = new (std::nothrow) PageTable(std::move(tableBlock), 0, self->currentPageNextY, tableHeight);
    if (!ptRaw) {
      LOG_ERR("EHP", "OOM: PageTable");
      return;
    }
    self->currentPage->elements.push_back(std::shared_ptr<PageTable>(ptRaw));
    self->currentPageNextY += tableHeight;

    rowStart = rowEnd;

    if (rowStart < totalRows) {
      self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex, self->xpathListItemIndex);
      self->completedPageCount++;
      self->currentPage.reset();
      self->currentPageNextY = 0;
    }
  }

  if (rowStart > 0) {
    self->tableAccumRows.erase(self->tableAccumRows.begin(),
                               self->tableAccumRows.begin() + static_cast<ptrdiff_t>(rowStart));
    self->tableHasFlushedRows = true;
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

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
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    // Move the accumulated cell ParsedText into the cell accumulator for later layout
    if (!self->tableAccumRows.empty()) {
      auto& cell = self->tableAccumRows.back().cells.back();
      cell.text = std::move(self->currentTextBlock);
    }
    self->inTableCellMode = false;
    // Reset currentTextBlock for any content between </td> and next <td>/<tr>
    auto interCellText =
        makeUniqueNoThrow<ParsedText>(self->extraParagraphSpacing, self->hyphenationEnabled, self->focusReadingEnabled);
    if (!interCellText) {
      LOG_ERR("EHP", "OOM: ParsedText");
      return;
    }
    self->currentTextBlock = std::move(interCellText);
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    // Layout all cells in this row now that we know the column count
    if (!self->tableAccumRows.empty() && !self->tableTruncated) {
      auto& row = self->tableAccumRows.back();

      // Interleave phantom cells for columns occupied by rowspan cells from previous rows.
      {
        std::vector<TableCellAccum> interleaved;
        interleaved.reserve(row.cells.size() + self->tableRowspanTracker.size());
        uint8_t lc = 0;
        for (auto& ca : row.cells) {
          // Insert a phantom for each occupied column before this cell.
          while (lc < static_cast<uint8_t>(self->tableRowspanTracker.size()) && self->tableRowspanTracker[lc] > 0) {
            TableCellAccum phantom;
            phantom.rowspan = 0;
            interleaved.push_back(std::move(phantom));
            self->tableRowspanTracker[lc]--;
            lc++;
          }
          // If this cell has rowspan > 1, mark covered columns for future rows.
          if (ca.rowspan > 1) {
            for (uint8_t k = 0; k < ca.colspan; k++) {
              const uint8_t c = static_cast<uint8_t>(lc + k);
              if (c >= static_cast<uint8_t>(self->tableRowspanTracker.size()))
                self->tableRowspanTracker.resize(c + 1, 0);
              self->tableRowspanTracker[c] = ca.rowspan - 1;
            }
          }
          lc = static_cast<uint8_t>(std::min(255, lc + ca.colspan));
          interleaved.push_back(std::move(ca));
        }
        // Trailing phantom columns after all actual cells.
        while (lc < static_cast<uint8_t>(self->tableRowspanTracker.size()) && self->tableRowspanTracker[lc] > 0) {
          TableCellAccum phantom;
          phantom.rowspan = 0;
          interleaved.push_back(std::move(phantom));
          self->tableRowspanTracker[lc]--;
          lc++;
        }
        row.cells = std::move(interleaved);
      }

      int logicalColCount = 0;
      for (const auto& ca : row.cells) logicalColCount += ca.colspan;
      const auto numCols = static_cast<uint8_t>(std::min(logicalColCount, 255));
      if (numCols > self->tableTotalCols) {
        self->tableTotalCols = numCols;
      }

      // Determine per-column widths for this row.
      // Only colspan=1 cells with an explicit requestedWidth contribute to column width establishment;
      // spanning cells' widths are ambiguous and skipped.
      if (self->tableColWidths.empty() && numCols > 0) {
        bool anyExplicit = false;
        for (const auto& ca : row.cells) {
          if (ca.colspan == 1 && ca.requestedWidth > 0) {
            anyExplicit = true;
            break;
          }
        }
        if (anyExplicit) {
          // Build per-logical-column widths, distributing remainder to unspecified slots.
          std::vector<int16_t> slotWidths(numCols, 0);
          int16_t totalExplicit = 0;
          int unspecifiedCount = 0;
          uint8_t logCol = 0;
          for (const auto& ca : row.cells) {
            if (ca.colspan == 1 && ca.requestedWidth > 0) {
              if (logCol < numCols) {
                // CSS width is content-box; column width must include horizontal padding
                // so that textWidth = cellColWidth - 1 - padL - padR ≈ requestedWidth - 1
                const int16_t colW = static_cast<int16_t>(ca.requestedWidth + ca.paddingLeft + ca.paddingRight);
                slotWidths[logCol] = colW;
                totalExplicit += colW;
              }
            } else {
              // Spanning or unspecified: leave slots as 0 (unspecified).
              for (uint8_t k = 0; k < ca.colspan && (logCol + k) < numCols; k++) {
                ++unspecifiedCount;
              }
            }
            logCol = static_cast<uint8_t>(std::min(255, logCol + ca.colspan));
          }
          const int16_t remaining = static_cast<int16_t>(self->viewportWidth - totalExplicit);
          const int16_t fallback = (unspecifiedCount > 0)
                                       ? static_cast<int16_t>(std::max(1, remaining / unspecifiedCount))
                                       : static_cast<int16_t>(self->viewportWidth / numCols);
          self->tableColWidths.reserve(numCols);
          for (auto w : slotWidths) {
            self->tableColWidths.push_back(w > 0 ? w : fallback);
          }
          // Scale columns proportionally to fit viewport
          // (must happen here so text layout uses the scaled widths)
          int16_t totalColW = 0;
          for (auto w : self->tableColWidths) totalColW += w;
          if (totalColW > 0 && totalColW != self->viewportWidth) {
            const float scale = static_cast<float>(self->viewportWidth) / totalColW;
            for (auto& w : self->tableColWidths) {
              w = static_cast<int16_t>(std::max(1.0f, w * scale));
            }
          }
        }
      }

      {
        uint8_t logCol = 0;
        for (auto& ca : row.cells) {
          // Phantom cells (rowspan==0) occupy one column with no content — skip layout.
          if (ca.rowspan == 0) {
            logCol++;
            continue;
          }
          int16_t cellColWidth = 0;
          for (uint8_t k = 0; k < ca.colspan; k++) {
            const uint8_t c = logCol + k;
            if (c < static_cast<uint8_t>(self->tableColWidths.size())) {
              cellColWidth = static_cast<int16_t>(cellColWidth + self->tableColWidths[c]);
            } else if (numCols > 0) {
              cellColWidth = static_cast<int16_t>(cellColWidth + self->viewportWidth / numCols);
            } else {
              cellColWidth = static_cast<int16_t>(cellColWidth + self->viewportWidth);
            }
          }
          // CSS width is content-box; subtract left border (1px) + padding; ensure at least 1px.
          const int16_t textWidth =
              static_cast<int16_t>(std::max(1, cellColWidth - 1 - ca.paddingLeft - ca.paddingRight));

          if (ca.text && !ca.text->isEmpty()) {
            ca.text->layoutAndExtractLines(self->renderer, self->fontId, textWidth,
                                           [&ca](const std::shared_ptr<TextBlock>& line) { ca.lines.push_back(line); });
          }
          ca.text.reset();  // raw word buffer no longer needed after layout; free it now

          logCol = static_cast<uint8_t>(std::min(255, logCol + ca.colspan));
        }
      }
      recomputeRowMetrics(row);

      // Incremental page flush — emit complete pages now rather than waiting for </table>.
      // Only safe when no rowspans are pending (rowspan distribution at </table> won't need to
      // reach back into already-flushed rows). Column widths must also be established so early
      // TableBlocks use the same numCols as later ones.
      const bool noActiveRowspans = self->tableRowspanTracker.empty() ||
                                    std::all_of(self->tableRowspanTracker.begin(), self->tableRowspanTracker.end(),
                                                [](uint8_t v) { return v == 0; });
      const bool colWidthsEstablished =
          !self->tableColWidths.empty() && static_cast<int>(self->tableColWidths.size()) == self->tableTotalCols;
      if (noActiveRowspans && colWidthsEstablished) {
        const auto lhCheck = static_cast<int16_t>(self->renderer.getLineHeight(self->fontId) * self->lineCompression);
        int32_t accumH = 1;  // top border
        for (const auto& r : self->tableAccumRows) {
          accumH += 1 + r.maxLines * lhCheck + r.maxPaddingTop + r.maxPaddingBottom;
        }
        const int32_t pageAvail = self->viewportHeight - self->currentPageNextY;
        if (accumH > pageAvail) {
          emitTablePageSlices(self, !self->tableHasFlushedRows, false, false);
        }
      }
    }
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->nextWordContinues = false;

    if (!self->tableAccumRows.empty() && self->tableTotalCols > 0) {
      // Distribute spanning cell lines evenly across the rows they span.
      // Without this, all content sits in the first row, making it extremely
      // tall while spanned rows are tiny — causing uneven heights in adjacent
      // columns and poor page splits.
      for (size_t ri = 0; ri < self->tableAccumRows.size(); ri++) {
        uint8_t logCol = 0;
        for (auto& ca : self->tableAccumRows[ri].cells) {
          if (ca.rowspan > 1) {
            const size_t endRow = std::min(ri + static_cast<size_t>(ca.rowspan), self->tableAccumRows.size());
            const size_t numSpanned = endRow - ri;
            if (!ca.lines.empty() && numSpanned > 1) {
              const uint8_t linesPerRow =
                  std::max(uint8_t(1), static_cast<uint8_t>((ca.lines.size() + numSpanned - 1) / numSpanned));
              const uint8_t targetLogCol = logCol;
              std::vector<std::shared_ptr<TextBlock>> allLines = std::move(ca.lines);
              std::vector<std::shared_ptr<TextBlock>> chunk;
              chunk.reserve(linesPerRow);
              size_t srcIdx = 0;
              for (size_t i = 0; i < numSpanned && srcIdx < allLines.size(); i++) {
                const size_t r = ri + i;
                const uint8_t count = std::min(linesPerRow, static_cast<uint8_t>(allLines.size() - srcIdx));
                chunk.clear();
                for (uint8_t j = 0; j < count; j++) {
                  chunk.push_back(std::move(allLines[srcIdx++]));
                }
                if (i == 0) {
                  ca.lines = std::move(chunk);
                } else {
                  uint8_t plc = 0;
                  for (auto& pca : self->tableAccumRows[r].cells) {
                    if (plc == targetLogCol && pca.rowspan == 0) {
                      pca.lines = std::move(chunk);
                      pca.paddingLeft = ca.paddingLeft;
                      pca.paddingRight = ca.paddingRight;
                      pca.paddingTop = ca.paddingTop;
                      pca.paddingBottom = ca.paddingBottom;
                      break;
                    }
                    plc = static_cast<uint8_t>(plc + ((pca.rowspan == 0) ? 1 : pca.colspan));
                  }
                }
              }
            }
          }
          logCol = static_cast<uint8_t>(logCol + ((ca.rowspan == 0) ? 1 : ca.colspan));
        }
      }

      for (auto& row : self->tableAccumRows) recomputeRowMetrics(row);

      const auto numCols = static_cast<uint8_t>(self->tableTotalCols);
      const bool hasPerColWidths = (static_cast<int>(self->tableColWidths.size()) == numCols);
      const auto colWidthFallback =
          static_cast<int16_t>(numCols > 0 ? self->viewportWidth / numCols : self->viewportWidth);
      const auto lh = static_cast<int16_t>(self->renderer.getLineHeight(self->fontId) * self->lineCompression);

      if (numCols > TableBlock::MAX_COLS) {
        LOG_ERR("EHP", "Table too wide (%u cols), rendering placeholder", numCols);
        if (!self->currentPage) {
          self->currentPage = makeUniqueNoThrow<Page>();
          if (!self->currentPage) {
            LOG_ERR("EHP", "OOM: Page");
            return;
          }
          self->currentPageNextY = 0;
        }
        std::vector<std::string> words = {"[Table too wide]"};
        std::vector<int16_t> xpos = {0};
        std::vector<EpdFontFamily::Style> styles = {EpdFontFamily::REGULAR};
        BlockStyle placeholderStyle;
        placeholderStyle.textAlignDefined = true;
        placeholderStyle.alignment = CssTextAlign::Center;

        auto placeholder =
            std::make_shared<TextBlock>(std::move(words), std::move(xpos), std::move(styles), std::vector<uint8_t>{},
                                        std::vector<uint16_t>{}, placeholderStyle);
        self->currentPage->elements.push_back(std::make_shared<PageLine>(placeholder, 0, self->currentPageNextY));
        self->currentPageNextY += lh;

        self->tableAccumRows.clear();
      }

      emitTablePageSlices(self, !self->tableHasFlushedRows, true, true);
    }

    self->tableAccumRows.clear();
    self->tableTotalCols = 0;
    self->tableColWidths.clear();
    self->tableRowspanTracker.clear();
    self->tableTruncated = false;
    self->tableHasFlushedRows = false;
    self->tableDrawBorderTop = false;
    self->tableDrawBorderBottom = false;
    self->tableDrawBorderLeft = false;
    self->tableDrawBorderRight = false;
    self->tableDrawInnerColDividers = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
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
      self->blockStyleStack.pop_back();
    }
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
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

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  HalFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    destroyXmlParser(parser);
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Compute the time taken to parse and build pages
  const uint32_t chapterStartTime = millis();
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, PARSE_BUFFER_SIZE);

    if (len == 0 && file.available() > 0) {
      LOG_ERR("EHP", "File read error");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      file.close();
      return false;
    }
  } while (!done);
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - chapterStartTime);

  destroyXmlParser(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

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
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

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
