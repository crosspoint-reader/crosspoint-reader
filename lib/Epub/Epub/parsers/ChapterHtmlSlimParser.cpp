#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HardwareSerial.h>
#include <expat.h>

#include "../Page.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;

  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) {
      return atts[i + 1];
    }
  }
  return nullptr;
}

// Simple HTML entity replacement for noteref text
std::string replaceHtmlEntities(const char* text) {
  if (!text) return "";
  std::string s(text);

  size_t pos = 0;
  while (pos < s.length()) {
    if (s[pos] == '&') {
      bool replaced = false;
      const char* ptr = s.c_str() + pos;  // Get pointer to current position (no allocation)

      if (pos + 1 < s.length()) {
        switch (s[pos + 1]) {
          case 'l':  // &lt;
            if (pos + 3 < s.length() && strncmp(ptr, "&lt;", 4) == 0) {
              s.replace(pos, 4, "<");
              replaced = true;
            }
            break;

          case 'g':  // &gt;
            if (pos + 3 < s.length() && strncmp(ptr, "&gt;", 4) == 0) {
              s.replace(pos, 4, ">");
              replaced = true;
            }
            break;

          case 'a':  // &amp; or &apos;
            if (pos + 4 < s.length() && strncmp(ptr, "&amp;", 5) == 0) {
              s.replace(pos, 5, "&");
              replaced = true;
            } else if (pos + 5 < s.length() && strncmp(ptr, "&apos;", 6) == 0) {
              s.replace(pos, 6, "'");
              replaced = true;
            }
            break;

          case 'q':  // &quot;
            if (pos + 5 < s.length() && strncmp(ptr, "&quot;", 6) == 0) {
              s.replace(pos, 6, "\"");
              replaced = true;
            }
            break;
        }
      }

      // Don't increment pos if we replaced - allows nested entity handling
      // Example: &amp;lt; -> &lt; (iteration 1) -> < (iteration 2)
      if (!replaced) {
        pos++;
      }
    } else {
      pos++;
    }
  }

  return s;
}

// Check if href points to internal EPUB location (not external URL)
bool isInternalEpubLink(const char* href) {
  if (!href) return false;

  switch (href[0]) {
    case 'h':  // http/https
      if (strncmp(href, "http", 4) == 0) return false;
    case 'f':  // ftp
      if (strncmp(href, "ftp://", 6) == 0) return false;
    case 'm':  // mailto
      if (strncmp(href, "mailto:", 7) == 0) return false;
    case 't':  // tel
      if (strncmp(href, "tel:", 4) == 0) return false;
    case 's':  // sms
      if (strncmp(href, "sms:", 4) == 0) return false;
  }
  // Everything else is internal (relative paths, anchors, etc.)
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;

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
  }
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

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(replaceHtmlEntities(partWordBuffer), fontStyle, nullptr, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      currentTextBlock->setBlockStyle(currentTextBlock->getBlockStyle().getCombinedBlockStyle(blockStyle));
      return;
    }
    makePages();
  }
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, blockStyle));
}

std::unique_ptr<FootnoteEntry> ChapterHtmlSlimParser::createFootnoteEntry(const char* number, const char* href) {
  auto entry = std::unique_ptr<FootnoteEntry>(new FootnoteEntry());

  Serial.printf("[%lu] [ADDFT] Creating footnote: num=%s, href=%s\n", millis(), number, href);

  // Copy number
  strncpy(entry->number, number, sizeof(entry->number) - 1);
  entry->number[sizeof(entry->number) - 1] = '\0';

  // Check if this is an inline footnote reference
  const char* hashPos = strchr(href, '#');
  if (hashPos) {
    const char* inlineId = hashPos + 1;  // Skip the '#'

    // Check if we have this inline footnote
    bool foundInline = false;
    for (int i = 0; i < inlineFootnoteCount; i++) {
      if (strcmp(inlineFootnotes[i].id, inlineId) == 0) {
        // This is an inline footnote! Rewrite the href
        char rewrittenHref[64];
        snprintf(rewrittenHref, sizeof(rewrittenHref), "inline_%s.html#%s", inlineId, inlineId);

        strncpy(entry->href, rewrittenHref, 63);
        entry->href[63] = '\0';

        Serial.printf("[%lu] [ADDFT] Rewrote inline href to: %s\n", millis(), rewrittenHref);
        foundInline = true;
        break;
      }
    }

    // Check if we have this as a paragraph note
    if (!foundInline) {
      for (int i = 0; i < paragraphNoteCount; i++) {
        if (strcmp(paragraphNotes[i].id, inlineId) == 0) {
          char rewrittenHref[64];
          snprintf(rewrittenHref, sizeof(rewrittenHref), "pnote_%s.html#%s", inlineId, inlineId);

          strncpy(entry->href, rewrittenHref, 63);
          entry->href[63] = '\0';

          Serial.printf("[%lu] [ADDFT] Rewrote paragraph note href to: %s\n", millis(), rewrittenHref);
          foundInline = true;
          break;
        }
      }
    }

    if (!foundInline) {
      // Normal href, just copy it
      strncpy(entry->href, href, 63);
      entry->href[63] = '\0';
    }
  } else {
    // No anchor, just copy
    strncpy(entry->href, href, 63);
    entry->href[63] = '\0';
  }

  Serial.printf("[%lu] [ADDFT] Created as: num=%s, href=%s\n", millis(), entry->number, entry->href);
  return entry;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // ============================================================================
  // PASS 1: Detect and collect <p class="note">
  // ============================================================================
  if (strcmp(name, "p") == 0 && self->isPass1CollectingAsides) {
    const char* classAttr = getAttribute(atts, "class");

    if (classAttr && (strcmp(classAttr, "note") == 0 || strstr(classAttr, "note"))) {
      self->insideParagraphNote = true;
      self->paragraphNoteDepth = self->depth;
      self->currentParagraphNoteTextLen = 0;
      self->currentParagraphNoteText[0] = '\0';
      self->currentParagraphNoteId[0] = '\0';

      self->depth += 1;
      return;
    }
  }

  // Inside paragraph note in Pass 1, look for <a id="rnoteX">
  if (self->insideParagraphNote && self->isPass1CollectingAsides && strcmp(name, "a") == 0) {
    const char* id = getAttribute(atts, "id");

    if (id && strncmp(id, "rnote", 5) == 0) {
      strncpy(self->currentParagraphNoteId, id, sizeof(self->currentParagraphNoteId) - 1);
      self->currentParagraphNoteId[sizeof(self->currentParagraphNoteId) - 1] = '\0';
    }

    self->depth += 1;
    return;
  }

  // ============================================================================
  // PASS 1: Detect and collect <aside epub:type="footnote">
  // ============================================================================
  if (strcmp(name, "aside") == 0) {
    const char* epubType = getAttribute(atts, "epub:type");
    const char* id = getAttribute(atts, "id");

    if (epubType && strcmp(epubType, "footnote") == 0 && id) {
      if (self->isPass1CollectingAsides) {
        // Pass 1: Collect aside
        self->insideAsideFootnote = true;
        self->asideDepth = self->depth;
        self->currentAsideTextLen = 0;
        self->currentAsideText[0] = '\0';

        strncpy(self->currentAsideId, id, sizeof(self->currentAsideId) - 1);
        self->currentAsideId[sizeof(self->currentAsideId) - 1] = '\0';
      } else {
        // Pass 2: Skip the aside (we already have it from Pass 1)
        self->skipUntilDepth = self->depth;
      }
    }

    self->depth += 1;
    return;
  }

  // ============================================================================
  // PASS 2: FOOTNOTE DETECTION
  // ============================================================================
  if (!self->isPass1CollectingAsides && strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    // Flush pending word buffer before starting footnote
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    // Check for internal EPUB link
    bool isInternalLink = isInternalEpubLink(href);

    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
    }

    // If it's an internal link, treat it as a footnote
    if (isInternalLink && href) {
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      self->currentFootnoteLinkHref[0] = '\0';
      strncpy(self->currentFootnoteLinkHref, href, 63);
      self->currentFootnoteLinkHref[63] = '\0';

      self->currentFootnoteLinkText[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      self->depth += 1;
      return;
    }
  }

  // Extract class and style attributes for CSS processing
  std::string classAttr;
  std::string styleAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Special handling for tables - show placeholder text instead of dropping silently
  if (strcmp(name, "table") == 0) {
    // Add placeholder text
    self->startNewTextBlock(centeredBlockStyle);

    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Advance depth before processing character data (like you would for an element with text)
    self->depth += 1;
    self->characterData(userData, "[Table omitted]", strlen("[Table omitted]"));

    // Skip table contents (skip until parent as we pre-advanced depth above)
    self->skipUntilDepth = self->depth - 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // TODO: Start processing image tags
    std::string alt = "[Image]";
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "alt") == 0) {
          if (strlen(atts[i + 1]) > 0) {
            alt = "[Image: " + std::string(atts[i + 1]) + "]";
          }
          break;
        }
      }
    }

    Serial.printf("[%lu] [EHP] Image alt: %s\n", millis(), alt.c_str());

    self->startNewTextBlock(centeredBlockStyle);
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Advance depth before processing character data (like you would for an element with text)
    self->depth += 1;
    self->characterData(userData, alt.c_str(), alt.length());

    // Skip table contents (skip until parent as we pre-advanced depth above)
    self->skipUntilDepth = self->depth - 1;
    return;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
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

  // Compute CSS style for this element
  CssStyle cssStyle;
  if (self->cssParser) {
    // Get combined tag + class styles
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    // Merge inline style (highest priority)
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  const float emSize = static_cast<float>(self->renderer.getLineHeight(self->fontId)) * self->lineCompression;
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    self->startNewTextBlock(headerBlockStyle);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->currentTextBlock->getBlockStyle());
    } else {
      self->currentCssStyle = cssStyle;
      self->startNewTextBlock(userAlignmentBlockStyle);
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        // Use generic addWord with default args for bullet
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
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
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
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
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
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
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
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
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Collect paragraph note text in Pass 1
  if (self->insideParagraphNote && self->isPass1CollectingAsides) {
    for (int i = 0; i < len; i++) {
      if (self->currentParagraphNoteTextLen >= self->MAX_PNOTE_BUFFER - 2) {
        break;
      }

      unsigned char c = (unsigned char)s[i];

      if (isWhitespace(c)) {
        if (self->currentParagraphNoteTextLen > 0 &&
            self->currentParagraphNoteText[self->currentParagraphNoteTextLen - 1] != ' ') {
          self->currentParagraphNoteText[self->currentParagraphNoteTextLen++] = ' ';
        }
      } else if (c >= 32 || c >= 0x80) {  // Accept printable ASCII AND UTF-8
        self->currentParagraphNoteText[self->currentParagraphNoteTextLen++] = c;
      }
    }
    self->currentParagraphNoteText[self->currentParagraphNoteTextLen] = '\0';
    return;
  }

  // If inside aside, collect the text ONLY in pass 1
  if (self->insideAsideFootnote) {
    if (!self->isPass1CollectingAsides) {
      return;
    }

    for (int i = 0; i < len; i++) {
      if (self->currentAsideTextLen >= self->MAX_ASIDE_BUFFER - 2) {
        break;
      }

      unsigned char c = (unsigned char)s[i];  // Cast to unsigned char

      if (isWhitespace(c)) {
        if (self->currentAsideTextLen > 0 && self->currentAsideText[self->currentAsideTextLen - 1] != ' ') {
          self->currentAsideText[self->currentAsideTextLen++] = ' ';
        }
      } else if (c >= 32 || c >= 0x80) {  // Accept printable ASCII AND UTF-8 bytes
        self->currentAsideText[self->currentAsideTextLen++] = c;
      }
    }
    self->currentAsideText[self->currentAsideTextLen] = '\0';
    return;
  }

  // During pass 1, skip all other content
  if (self->isPass1CollectingAsides) {
    return;
  }

  // Rest of characterData logic for pass 2...
  if (self->insideFootnoteLink) {
    for (int i = 0; i < len; i++) {
      unsigned char c = (unsigned char)s[i];
      // Skip whitespace and brackets []
      if (!isWhitespace(c) && c != '[' && c != ']' && self->currentFootnoteLinkTextLen < 63) {
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen++] = c;
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen] = '\0';
      }
    }
    return;
  }

  if (self->skipUntilDepth < self->depth) {
    return;
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

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->size() > 750) {
    Serial.printf("[%lu] [EHP] Text block too long, splitting into multiple pages\n", millis());
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, self->viewportWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock, const std::vector<FootnoteEntry>& footnotes) {
          self->addLineToPage(textBlock, footnotes);
        },
        false);
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // ============================================================================
  // PASS 1: End of <aside epub:type="footnote">
  // ============================================================================
  if (strcmp(name, "aside") == 0 && self->insideAsideFootnote && self->depth == self->asideDepth + 1) {
    if (self->isPass1CollectingAsides) {
      // Store the inline footnote
      if (self->inlineFootnoteCount < 16) {
        InlineFootnote& fn = self->inlineFootnotes[self->inlineFootnoteCount];

        strncpy(fn.id, self->currentAsideId, sizeof(fn.id) - 1);
        fn.id[sizeof(fn.id) - 1] = '\0';

        // Allocate memory for text
        fn.text = (char*)malloc(self->currentAsideTextLen + 1);
        if (fn.text) {
          strncpy(fn.text, self->currentAsideText, self->currentAsideTextLen);
          fn.text[self->currentAsideTextLen] = '\0';
        }

        self->inlineFootnoteCount++;

        Serial.printf("[%lu] [ASIDE] Stored inline footnote: id=%s\n", millis(), fn.id);
      }
    }

    self->insideAsideFootnote = false;
    self->currentAsideTextLen = 0;
    self->currentAsideText[0] = '\0';
  }

  // ============================================================================
  // PASS 1: End of <p class="note">
  // ============================================================================
  if (strcmp(name, "p") == 0 && self->insideParagraphNote && self->depth == self->paragraphNoteDepth + 1) {
    if (self->isPass1CollectingAsides && self->currentParagraphNoteId[0] != '\0') {
      // Store the paragraph note
      if (self->paragraphNoteCount < 16) {
        ParagraphNote& pn = self->paragraphNotes[self->paragraphNoteCount];

        strncpy(pn.id, self->currentParagraphNoteId, sizeof(pn.id) - 1);
        pn.id[sizeof(pn.id) - 1] = '\0';

        // Allocate memory
        pn.text = (char*)malloc(self->currentParagraphNoteTextLen + 1);
        if (pn.text) {
          strncpy(pn.text, self->currentParagraphNoteText, self->currentParagraphNoteTextLen);
          pn.text[self->currentParagraphNoteTextLen] = '\0';
        }

        self->paragraphNoteCount++;

        Serial.printf("[%lu] [PNOTE] Stored paragraph note: id=%s\n", millis(), pn.id);
      }
    }

    self->insideParagraphNote = false;
    self->currentParagraphNoteTextLen = 0;
    self->currentParagraphNoteText[0] = '\0';
    self->currentParagraphNoteId[0] = '\0';
  }

  // ============================================================================
  // PASS 2: End of footnote link
  // ============================================================================
  if (!self->isPass1CollectingAsides && strcmp(name, "a") == 0 && self->insideFootnoteLink &&
      self->depth == self->footnoteLinkDepth + 1) {
    if (self->currentFootnoteLinkText[0] != '\0' && self->currentFootnoteLinkHref[0] != '\0') {
      if (self->currentTextBlock) {
        auto footnote = self->createFootnoteEntry(self->currentFootnoteLinkText, self->currentFootnoteLinkHref);

        // Invoke the noteref callback if set
        if (self->noterefCallback && footnote) {
          Noteref noteref;
          strncpy(noteref.number, footnote->number, sizeof(noteref.number) - 1);
          noteref.number[sizeof(noteref.number) - 1] = '\0';
          strncpy(noteref.href, footnote->href, sizeof(noteref.href) - 1);
          noteref.href[sizeof(noteref.href) - 1] = '\0';
          self->noterefCallback(noteref);
        }

        // Format the noteref text with brackets
        char formattedNoteref[32];
        snprintf(formattedNoteref, sizeof(formattedNoteref), "[%s]", self->currentFootnoteLinkText);

        // Determine font style from CSS effective style
        const bool isBold = self->effectiveBold;
        const bool isItalic = self->effectiveItalic;
        const bool isUnderline = self->effectiveUnderline;

        EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
        if (isBold) fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
        if (isItalic) fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
        if (isUnderline) fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);

        // Pass false for underline/attachToPrevious args as they are defaults or handled in style
        self->currentTextBlock->addWord(formattedNoteref, fontStyle, std::move(footnote), false, false);
      }
    }

    self->insideFootnoteLink = false;
    self->currentFootnoteLinkTextLen = 0;
    self->currentFootnoteLinkText[0] = '\0';
    self->currentFootnoteLinkHref[0] = '\0';
  }

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

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && strcmp(name, "table") != 0 &&
                             !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || strcmp(name, "table") == 0 ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
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
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  // ============================================================================
  // PASS 1: Extract all inline footnotes (aside elements) FIRST
  // ============================================================================
  Serial.printf("[%lu] [PARSER] === PASS 1: Extracting inline footnotes ===\n", millis());

  // Reset state for pass 1
  depth = 0;
  skipUntilDepth = INT_MAX;
  boldUntilDepth = INT_MAX;
  italicUntilDepth = INT_MAX;
  underlineUntilDepth = INT_MAX;
  insideAsideFootnote = false;
  insideParagraphNote = false;
  inlineFootnoteCount = 0;
  paragraphNoteCount = 0;
  isPass1CollectingAsides = true;

  XML_Parser parser1 = XML_ParserCreate(nullptr);
  if (!parser1) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  XML_SetUserData(parser1, this);
  XML_SetElementHandler(parser1, startElement, endElement);
  XML_SetCharacterDataHandler(parser1, characterData);

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser1);
    return false;
  }

  bool done = false;
  do {
    void* const buf = XML_GetBuffer(parser1, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_ParserFree(parser1);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_ParserFree(parser1);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser1, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser1),
                    XML_ErrorString(XML_GetErrorCode(parser1)));
      XML_ParserFree(parser1);
      file.close();
      return false;
    }
  } while (!done);

  XML_ParserFree(parser1);
  file.close();

  Serial.printf("[%lu] [PARSER] Pass 1 complete: found %d inline footnotes\n", millis(), inlineFootnoteCount);

  // ============================================================================
  // PASS 2: Build pages with inline footnotes already available
  // ============================================================================
  Serial.printf("[%lu] [PARSER] === PASS 2: Building pages ===\n", millis());

  // Reset parser state for pass 2
  depth = 0;
  skipUntilDepth = INT_MAX;
  boldUntilDepth = INT_MAX;
  italicUntilDepth = INT_MAX;
  underlineUntilDepth = INT_MAX;
  partWordBufferIndex = 0;
  insideAsideFootnote = false;
  insideFootnoteLink = false;
  isPass1CollectingAsides = false;

  // Clear style stacks for Pass 2
  inlineStyleStack.clear();
  currentCssStyle.reset();
  effectiveBold = false;
  effectiveItalic = false;
  effectiveUnderline = false;

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  if (!Storage.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();

    if (currentPage) {
      completePageFn(std::move(currentPage));
    }

    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line,
                                          const std::vector<FootnoteEntry>& footnotes) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (lineHeight > viewportHeight) {
    Serial.printf("[%lu] [EHP] WARNING: Line taller than viewport\n", millis());
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (!currentPage) return;

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));

  currentPageNextY += lineHeight;

  for (const auto& fn : footnotes) {
    currentPage->addFootnote(fn.number, fn.href);
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    Serial.printf("[%lu] [EHP] !! No text block to make pages for !!\n", millis());
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
      [this](const std::shared_ptr<TextBlock>& textBlock, const std::vector<FootnoteEntry>& footnotes) {
        addLineToPage(textBlock, footnotes);
      });

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