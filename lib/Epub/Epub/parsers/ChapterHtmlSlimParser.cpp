#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

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

  // Replace common entities
  size_t pos = 0;
  while ((pos = s.find("&lt;", pos)) != std::string::npos) {
    s.replace(pos, 4, "<");
    pos += 1;
  }
  pos = 0;
  while ((pos = s.find("&gt;", pos)) != std::string::npos) {
    s.replace(pos, 4, ">");
    pos += 1;
  }
  pos = 0;
  while ((pos = s.find("&amp;", pos)) != std::string::npos) {
    s.replace(pos, 5, "&");
    pos += 1;
  }
  pos = 0;
  while ((pos = s.find("&quot;", pos)) != std::string::npos) {
    s.replace(pos, 6, "\"");
    pos += 1;
  }
  pos = 0;
  while ((pos = s.find("&apos;", pos)) != std::string::npos) {
    s.replace(pos, 6, "'");
    pos += 1;
  }
  return s;
}

EpdFontFamily::Style ChapterHtmlSlimParser::getCurrentFontStyle() const {
  if (boldUntilDepth < depth && italicUntilDepth < depth) {
    return EpdFontFamily::BOLD_ITALIC;
  } else if (boldUntilDepth < depth) {
    return EpdFontFamily::BOLD;
  } else if (italicUntilDepth < depth) {
    return EpdFontFamily::ITALIC;
  }
  return EpdFontFamily::REGULAR;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  EpdFontFamily::Style fontStyle = getCurrentFontStyle();
  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(std::move(replaceHtmlEntities(partWordBuffer)), fontStyle);
  partWordBufferIndex = 0;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::Style style) {
  if (currentTextBlock) {
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      return;
    }
    makePages();
  }
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing, hyphenationEnabled));
}

void ChapterHtmlSlimParser::addFootnoteToCurrentPage(const char* number, const char* href) {
  if (currentPageFootnoteCount >= 16) return;

  Serial.printf("[%lu] [ADDFT] Adding footnote: num=%s, href=%s\n", millis(), number, href);

  // Copy number
  strncpy(currentPageFootnotes[currentPageFootnoteCount].number, number, 2);
  currentPageFootnotes[currentPageFootnoteCount].number[2] = '\0';

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

        strncpy(currentPageFootnotes[currentPageFootnoteCount].href, rewrittenHref, 63);
        currentPageFootnotes[currentPageFootnoteCount].href[63] = '\0';

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

          strncpy(currentPageFootnotes[currentPageFootnoteCount].href, rewrittenHref, 63);
          currentPageFootnotes[currentPageFootnoteCount].href[63] = '\0';

          Serial.printf("[%lu] [ADDFT] Rewrote paragraph note href to: %s\n", millis(), rewrittenHref);
          foundInline = true;
          break;
        }
      }
    }

    if (!foundInline) {
      // Normal href, just copy it
      strncpy(currentPageFootnotes[currentPageFootnoteCount].href, href, 63);
      currentPageFootnotes[currentPageFootnoteCount].href[63] = '\0';
    }
  } else {
    // No anchor, just copy
    strncpy(currentPageFootnotes[currentPageFootnoteCount].href, href, 63);
    currentPageFootnotes[currentPageFootnoteCount].href[63] = '\0';
  }

  currentPageFootnoteCount++;

  Serial.printf("[%lu] [ADDFT] Stored as: num=%s, href=%s\n", millis(),
                currentPageFootnotes[currentPageFootnoteCount - 1].number,
                currentPageFootnotes[currentPageFootnoteCount - 1].href);
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // ============================================================================
  // PASS 1: Detect and collect <p class="note">
  // ============================================================================
  if (strcmp(name, "p") == 0 && self->isPass1CollectingAsides) {
    const char* classAttr = getAttribute(atts, "class");

    if (classAttr && (strcmp(classAttr, "note") == 0 || strstr(classAttr, "note"))) {
      Serial.printf("[%lu] [PNOTE] Found paragraph note (pass1=1)\n", millis());

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
      strncpy(self->currentParagraphNoteId, id, 15);
      self->currentParagraphNoteId[15] = '\0';
      Serial.printf("[%lu] [PNOTE] Found note ID: %s\n", millis(), id);
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
        Serial.printf("[%lu] [ASIDE] Found inline footnote: id=%s (pass1=%d)\n", millis(), id,
                      self->isPass1CollectingAsides);

        self->insideAsideFootnote = true;
        self->asideDepth = self->depth;
        self->currentAsideTextLen = 0;
        self->currentAsideText[0] = '\0';

        strncpy(self->currentAsideId, id, 2);
        self->currentAsideId[2] = '\0';
      } else {
        // Pass 2: Skip the aside (we already have it from Pass 1)
        Serial.printf("[%lu] [ASIDE] Skipping aside in Pass 2: id=%s\n", millis(), id);

        // Find the inline footnote text
        for (int i = 0; i < self->inlineFootnoteCount; i++) {
          if (strcmp(self->inlineFootnotes[i].id, id) == 0 && self->inlineFootnotes[i].text) {
            // Output the footnote text as normal text
            const char* text = self->inlineFootnotes[i].text;
            int textLen = strlen(text);

            // Process it through characterData
            self->characterData(self, text, textLen);

            Serial.printf("[%lu] [ASIDE] Rendered aside text: %.80s...\n", millis(), text);
            break;
          }
        }

        // Skip the aside element itself
        self->skipUntilDepth = self->depth;
      }

      self->depth += 1;
      return;
    }
  }

  // ============================================================================
  // PASS 1: Skip everything else
  // ============================================================================
  if (self->isPass1CollectingAsides) {
    self->depth += 1;
    return;
  }

  // ============================================================================
  // PASS 2: Skip <p class="note"> (we already have them from Pass 1)
  // ============================================================================
  if (strcmp(name, "p") == 0) {
    const char* classAttr = getAttribute(atts, "class");

    if (classAttr && (strcmp(classAttr, "note") == 0 || strstr(classAttr, "note"))) {
      Serial.printf("[%lu] [PNOTE] Skipping paragraph note in Pass 2\n", millis());
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  }

  // ============================================================================
  // PASS 2: Normal parsing
  // ============================================================================

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Rest of startElement logic for pass 2...
  if (strcmp(name, "sup") == 0) {
    self->supDepth = self->depth;

    // Case A: Found <sup> inside a normal <a> (which wasn't marked as a note yet)
    // Example: <a href="..."><sup>*</sup></a>
    if (self->anchorDepth != -1 && !self->insideNoteref) {
      Serial.printf("[%lu] [NOTEREF] Found <sup> inside <a>, promoting to noteref\n", millis());

      // Flush the current word buffer (text before the sup is normal text)
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      // Activate footnote mode
      self->insideNoteref = true;
      self->currentNoterefTextLen = 0;
      self->currentNoterefText[0] = '\0';
      // Note: The href was already saved to currentNoterefHref when the <a> was opened (see below)
    }
  }

  // === Update the existing A block ===
  if (strcmp(name, "a") == 0) {
    const char* epubType = getAttribute(atts, "epub:type");
    const char* href = getAttribute(atts, "href");

    // Save Anchor state
    self->anchorDepth = self->depth;

    // Optimistically save the href, in case this becomes a footnote later (via internal <sup>)
    if (!self->insideNoteref) {
      if (href) {
        strncpy(self->currentNoterefHref, href, 127);
        self->currentNoterefHref[127] = '\0';
      } else {
        self->currentNoterefHref[0] = '\0';
      }
    }

    // Footnote detection: via epub:type, rnote pattern, or if we are already inside a <sup>
    // Case B: Found <a> inside <sup>
    // Example: <sup><a href="...">1</a></sup>
    bool isNoteref = (epubType && strcmp(epubType, "noteref") == 0);

    if (!isNoteref && href && href[0] == '#' && strncmp(href + 1, "rnote", 5) == 0) {
      isNoteref = true;
    }

    // New detection: if we are inside SUP, this link is a footnote
    if (!isNoteref && self->supDepth != -1) {
      isNoteref = true;
      Serial.printf("[%lu] [NOTEREF] Found <a> inside <sup>, treating as noteref\n", millis());
    }

    if (isNoteref) {
      Serial.printf("[%lu] [NOTEREF] Found noteref: href=%s\n", millis(), href ? href : "null");
      // Flush word buffer
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->insideNoteref = true;
      self->currentNoterefTextLen = 0;
      self->currentNoterefText[0] = '\0';
      self->depth += 1;
      return;
    }
  }

  // Special handling for tables - show placeholder text instead of dropping silently
  if (strcmp(name, "table") == 0) {
    // Add placeholder text
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);

    self->italicUntilDepth = min(self->italicUntilDepth, self->depth);
    // Advance depth before processing character data (like you would for a element with text)
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

    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->italicUntilDepth = min(self->italicUntilDepth, self->depth);
    // Advance depth before processing character data (like you would for a element with text)
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

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->depth += 1;
    return;
  }

  if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->currentTextBlock->getStyle());
      self->depth += 1;
      return;
    }

    self->startNewTextBlock(static_cast<TextBlock::Style>(self->paragraphAlignment));
    if (strcmp(name, "li") == 0) {
      self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
    }

    self->depth += 1;
    return;
  }

  if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->depth += 1;
    return;
  }

  if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    self->depth += 1;
    return;
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
        if (self->currentParagraphNoteTextLen == self->MAX_PNOTE_BUFFER - 2) {
          Serial.printf("[%lu] [PNOTE] WARNING: Note text truncated at %d chars\n", millis(),
                        self->MAX_PNOTE_BUFFER - 2);
        }
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
        if (self->currentAsideTextLen == self->MAX_ASIDE_BUFFER - 2) {
          Serial.printf("[%lu] [ASIDE] WARNING: Footnote text truncated at %d chars (id=%s)\n", millis(),
                        self->MAX_ASIDE_BUFFER - 2, self->currentAsideId);
        }
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
      // Skip control characters (0x00-0x1F) except whitespace
    }
    self->currentAsideText[self->currentAsideTextLen] = '\0';
    return;
  }

  // During pass 1, skip all other content
  if (self->isPass1CollectingAsides) {
    return;
  }

  // Rest of characterData logic for pass 2...
  if (self->insideNoteref) {
    for (int i = 0; i < len; i++) {
      unsigned char c = (unsigned char)s[i];
      // Skip whitespace and brackets []
      if (!isWhitespace(c) && c != '[' && c != ']' && self->currentNoterefTextLen < 15) {
        self->currentNoterefText[self->currentNoterefTextLen++] = c;
        self->currentNoterefText[self->currentNoterefTextLen] = '\0';
      }
    }
    return;
  }

  if (self->skipUntilDepth < self->depth) {
    return;
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      continue;
    }

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
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
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Closing paragraph note in Pass 1
  if (strcmp(name, "p") == 0 && self->insideParagraphNote && self->depth - 1 == self->paragraphNoteDepth) {
    if (self->isPass1CollectingAsides && self->currentParagraphNoteTextLen > 0 && self->paragraphNoteCount < 32 &&
        self->currentParagraphNoteId[0] != '\0') {
      // Copy ID
      strncpy(self->paragraphNotes[self->paragraphNoteCount].id, self->currentParagraphNoteId, 15);
      self->paragraphNotes[self->paragraphNoteCount].id[15] = '\0';

      // Allocate memory for text
      size_t textLen = strlen(self->currentParagraphNoteText);
      self->paragraphNotes[self->paragraphNoteCount].text = static_cast<char*>(malloc(textLen + 1));

      if (self->paragraphNotes[self->paragraphNoteCount].text) {
        strcpy(self->paragraphNotes[self->paragraphNoteCount].text, self->currentParagraphNoteText);

        Serial.printf("[%lu] [PNOTE] Stored: %s -> %.80s... (allocated %d bytes)\n", millis(),
                      self->currentParagraphNoteId, self->currentParagraphNoteText, textLen + 1);

        self->paragraphNoteCount++;
      }
    }

    self->insideParagraphNote = false;
    self->depth -= 1;
    return;
  }

  // Closing aside - handle differently for Pass 1 vs Pass 2
  if (strcmp(name, "aside") == 0 && self->insideAsideFootnote && self->depth - 1 == self->asideDepth) {
    // Store footnote ONLY in Pass 1
    if (self->isPass1CollectingAsides && self->currentAsideTextLen > 0 && self->inlineFootnoteCount < 16) {
      // Copy ID (max 2 digits)
      strncpy(self->inlineFootnotes[self->inlineFootnoteCount].id, self->currentAsideId, 2);
      self->inlineFootnotes[self->inlineFootnoteCount].id[2] = '\0';

      // DYNAMIC ALLOCATION: allocate exactly the needed size + 1
      size_t textLen = strlen(self->currentAsideText);
      self->inlineFootnotes[self->inlineFootnoteCount].text = static_cast<char*>(malloc(textLen + 1));

      if (self->inlineFootnotes[self->inlineFootnoteCount].text) {
        strcpy(self->inlineFootnotes[self->inlineFootnoteCount].text, self->currentAsideText);

        Serial.printf("[%lu] [ASIDE] Stored: %s -> %.80s... (allocated %d bytes)\n", millis(), self->currentAsideId,
                      self->currentAsideText, textLen + 1);

        self->inlineFootnoteCount++;
      } else {
        Serial.printf("[%lu] [ASIDE] ERROR: Failed to allocate %d bytes for footnote %s\n", millis(), textLen + 1,
                      self->currentAsideId);
      }
    }

    // Reset state AFTER processing
    self->insideAsideFootnote = false;
    self->depth -= 1;
    return;
  }

  // During pass 1, skip all other processing
  if (self->isPass1CollectingAsides) {
    self->depth -= 1;
    return;
  }

  // ---------------------------------------------------------
  // PASS 2: Normal Parsing Logic
  // ---------------------------------------------------------

  // [NEW] 1. Reset Superscript State
  // We must ensure we know when we are leaving a <sup> tag
  if (strcmp(name, "sup") == 0) {
    if (self->supDepth == self->depth) {
      self->supDepth = -1;
    }
  }

  // [MODIFIED] 2. Handle 'a' tags (Anchors/Footnotes)
  // We check "a" generally now, to handle both Noterefs AND resetting regular links
  if (strcmp(name, "a") == 0) {
    // Track if this was a noteref so we can return early later
    bool wasNoteref = self->insideNoteref;

    if (self->insideNoteref) {
      self->insideNoteref = false;

      if (self->currentNoterefTextLen > 0) {
        Serial.printf("[%lu] [NOTEREF] %s -> %s\n", millis(), self->currentNoterefText, self->currentNoterefHref);

        // Add footnote first (this does the rewriting)
        self->addFootnoteToCurrentPage(self->currentNoterefText, self->currentNoterefHref);

        // Then call callback with the REWRITTEN href from currentPageFootnotes
        if (self->noterefCallback && self->currentPageFootnoteCount > 0) {
          Noteref noteref;
          strncpy(noteref.number, self->currentNoterefText, 15);
          noteref.number[15] = '\0';

          // Use the STORED href which has been rewritten
          FootnoteEntry* lastFootnote = &self->currentPageFootnotes[self->currentPageFootnoteCount - 1];
          strncpy(noteref.href, lastFootnote->href, 127);
          noteref.href[127] = '\0';

          self->noterefCallback(noteref);
        }

        // Ensure [1] appears inline after the word it references
        EpdFontFamily::Style fontStyle = self->getCurrentFontStyle();

        // Format the noteref text with brackets
        char formattedNoteref[32];
        snprintf(formattedNoteref, sizeof(formattedNoteref), "[%s]", self->currentNoterefText);

        // Add it as a word to the current text block
        if (self->currentTextBlock) {
          self->currentTextBlock->addWord(formattedNoteref, fontStyle);
        }
      }

      self->currentNoterefTextLen = 0;
      self->currentNoterefText[0] = '\0';
      self->currentNoterefHrefLen = 0;
      // Note: We do NOT clear currentNoterefHref here yet, we do it below
    }

    // [NEW] Reset Anchor Depth
    // This runs for BOTH footnotes and regular links to ensure state is clean
    if (self->anchorDepth == self->depth) {
      self->anchorDepth = -1;
      self->currentNoterefHref[0] = '\0';
    }

    // If it was a noteref, we are done with this tag, return early
    if (wasNoteref) {
      self->depth -= 1;
      return;
    }
  }

  if (self->partWordBufferIndex > 0) {
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
        strcmp(name, "table") == 0 || matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      self->flushPartWordBuffer();
    }
  }

  self->depth -= 1;

  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
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
  if (!SdMan.openFileForRead("EHP", filepath, file)) {
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
  for (int i = 0; i < inlineFootnoteCount; i++) {
    Serial.printf("[%lu] [PARSER]   - %s: %.80s\n", millis(), inlineFootnotes[i].id, inlineFootnotes[i].text);
  }

  // ============================================================================
  // PASS 2: Build pages with inline footnotes already available
  // ============================================================================
  Serial.printf("[%lu] [PARSER] === PASS 2: Building pages ===\n", millis());

  // Reset parser state for pass 2
  depth = 0;
  skipUntilDepth = INT_MAX;
  boldUntilDepth = INT_MAX;
  italicUntilDepth = INT_MAX;
  partWordBufferIndex = 0;
  insideNoteref = false;
  insideAsideFootnote = false;
  currentPageFootnoteCount = 0;
  isPass1CollectingAsides = false;

  supDepth = -1;
  anchorDepth = -1;

  startNewTextBlock((TextBlock::Style)this->paragraphAlignment);

  const XML_Parser parser2 = XML_ParserCreate(nullptr);
  if (!parser2) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  XML_SetUserData(parser2, this);
  XML_SetElementHandler(parser2, startElement, endElement);
  XML_SetCharacterDataHandler(parser2, characterData);

  if (!SdMan.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser2);
    return false;
  }

  // Get file size for progress calculation
  const size_t totalSize = file.size();
  size_t bytesRead = 0;
  int lastProgress = -1;

  do {
    void* const buf = XML_GetBuffer(parser2, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_ParserFree(parser2);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_StopParser(parser2, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser2, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser2, nullptr);
      XML_ParserFree(parser2);
      file.close();
      return false;
    }

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead += len;
    if (progressFn && totalSize >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead * 100) / totalSize);
      if (lastProgress / 10 != progress / 10) {
        lastProgress = progress;
        progressFn(progress);
      }
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser2, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser2),
                    XML_ErrorString(XML_GetErrorCode(parser2)));
      XML_StopParser(parser2, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser2, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser2, nullptr);
      XML_ParserFree(parser2);
      file.close();
      return false;
    }
  } while (!done);

  XML_ParserFree(parser2);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();

    if (currentPage) {
      for (int i = 0; i < currentPageFootnoteCount; i++) {
        currentPage->addFootnote(currentPageFootnotes[i].number, currentPageFootnotes[i].href);
      }
      currentPageFootnoteCount = 0;
      completePageFn(std::move(currentPage));
    }

    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    if (currentPage) {
      for (int i = 0; i < currentPageFootnoteCount; i++) {
        currentPage->addFootnote(currentPageFootnotes[i].number, currentPageFootnotes[i].href);
      }
      currentPageFootnoteCount = 0;
    }

    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPage && currentPage->elements.size() < 24) {  // Assuming generic capacity check or vector size
    currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageNextY));
    currentPageNextY += lineHeight;
  } else if (currentPage) {
    Serial.printf("[%lu] [EHP] WARNING: Page element capacity reached, skipping element\n", millis());
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
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  // Extra paragraph spacing if enabled
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
