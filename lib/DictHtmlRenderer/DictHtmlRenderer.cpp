#include "DictHtmlRenderer.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Tag classification
// ---------------------------------------------------------------------------

DictHtmlRenderer::TagAction DictHtmlRenderer::classify(const XML_Char* name) {
  // Block-strip: entire subtree discarded
  if (strcmp(name, "svg") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "hiero") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "math") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "gallery") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "nowiki") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "poem") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "ref") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "REF") == 0) return TagAction::BLOCK_STRIP;
  if (strcmp(name, "img") == 0) return TagAction::BLOCK_STRIP;

  // Children of block-strip tags (registered to avoid unknown-tag errors)
  if (strcmp(name, "defs") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "g") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "path") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "rect") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "use") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "table") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "tr") == 0) return TagAction::REGISTERED;
  if (strcmp(name, "td") == 0) return TagAction::REGISTERED;

  // Inline formatting
  if (strcmp(name, "b") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "strong") == 0) return TagAction::FORMAT_BOLD;
  if (strcmp(name, "i") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "em") == 0) return TagAction::FORMAT_ITALIC;
  if (strcmp(name, "u") == 0) return TagAction::FORMAT_UNDERLINE;
  if (strcmp(name, "s") == 0) return TagAction::FORMAT_STRIKE;
  if (strcmp(name, "sup") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "sub") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "code") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "tt") == 0) return TagAction::FORMAT_CODE;
  if (strcmp(name, "small") == 0) return TagAction::FORMAT_SMALL;
  if (strcmp(name, "big") == 0) return TagAction::FORMAT_SMALL;

  // Block structure
  if (strcmp(name, "p") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "div") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "br") == 0) return TagAction::BLOCK_BREAK;
  if (strcmp(name, "blockquote") == 0) return TagAction::BLOCK_QUOTE;

  // Lists
  if (strcmp(name, "li") == 0) return TagAction::LIST_ITEM;
  if (strcmp(name, "ol") == 0) return TagAction::LIST_CONTAINER;
  if (strcmp(name, "ul") == 0) return TagAction::LIST_CONTAINER;

  // Headings
  if (strcmp(name, "h1") == 0) return TagAction::HEADING;
  if (strcmp(name, "h2") == 0) return TagAction::HEADING;
  if (strcmp(name, "h3") == 0) return TagAction::HEADING;
  if (strcmp(name, "h4") == 0) return TagAction::HEADING;

  // Special
  if (strcmp(name, "abbr") == 0) return TagAction::ABBR;
  if (strcmp(name, "var") == 0) return TagAction::VAR;
  if (strcmp(name, "span") == 0) return TagAction::SPAN;
  if (strcmp(name, "a") == 0) return TagAction::SPAN;            // Registered; no actual anchors in dict
  if (strcmp(name, "_root") == 0) return TagAction::REGISTERED;  // Synthetic wrapper used by render()

  // Wikitext annotation tags: t:XX, tr:XX, lang:XX, gloss:XX, pos:XX, sc:XX, alt:XX, id:XX
  if (strncmp(name, "t:", 2) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "tr:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "lang:", 5) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "gloss:", 6) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "pos:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "sc:", 3) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "alt:", 4) == 0) return TagAction::WIKI_ANNOT;
  if (strncmp(name, "id:", 3) == 0) return TagAction::WIKI_ANNOT;

  return TagAction::STRIP_KEEP;  // Unknown — strip tag, keep text
}

const char* DictHtmlRenderer::findAttr(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Constructor / destructor / reset
// ---------------------------------------------------------------------------

DictHtmlRenderer::DictHtmlRenderer() { spans.reserve(64); }

DictHtmlRenderer::~DictHtmlRenderer() {
  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

void DictHtmlRenderer::reset() {
  spans.clear();
  textBufPos = 0;
  pendingLen = 0;
  stackDepth = 0;
  parseError = false;
  fmt = FormatState{};
  newlinePending = false;
  listItemPending = false;

  if (parser) {
    XML_ParserFree(parser);
    parser = nullptr;
  }
  parser = XML_ParserCreate(nullptr);

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
  unknownTagCount = 0;
#endif
}

// ---------------------------------------------------------------------------
// Text management
// ---------------------------------------------------------------------------

void DictHtmlRenderer::pushSpan() {
  if (pendingLen == 0) return;

  int spaceLeft = TEXT_BUF_SIZE - textBufPos - 1;
  if (spaceLeft <= 0) {
    pendingLen = 0;
    return;
  }

  int toWrite = pendingLen < spaceLeft ? pendingLen : spaceLeft;

  StyledSpan span;
  span.text = textBuf + textBufPos;
  span.bold = fmt.bold;
  span.italic = fmt.italic;
  span.underline = fmt.underline;
  span.strikethrough = fmt.strikethrough;
  span.indentLevel = fmt.indentLevel;
  span.newlineBefore = newlinePending;
  span.isListItem = listItemPending;

  memcpy(textBuf + textBufPos, pendingText, toWrite);
  textBuf[textBufPos + toWrite] = '\0';
  textBufPos += toWrite + 1;

  pendingLen = 0;
  newlinePending = false;
  listItemPending = false;

  spans.push_back(span);
}

void DictHtmlRenderer::emitText(const char* s, int len) {
  if (len <= 0) return;
  int remaining = PENDING_SIZE - pendingLen;
  if (len >= remaining) {
    pushSpan();
    remaining = PENDING_SIZE;
  }
  int toWrite = len < remaining - 1 ? len : remaining - 1;
  memcpy(pendingText + pendingLen, s, toWrite);
  pendingLen += toWrite;
}

void DictHtmlRenderer::flushPending() { pushSpan(); }

// ---------------------------------------------------------------------------
// Expat callbacks
// ---------------------------------------------------------------------------

void XMLCALL DictHtmlRenderer::onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  TagAction action = classify(name);

  // Propagate suppression from parent
  bool parentSuppresses = self->stackDepth > 0 && self->stack[self->stackDepth - 1].suppressChildren;

  StackEntry entry{};
  entry.action = action;
  entry.savedFmt = self->fmt;
  entry.suppressChildren = parentSuppresses;
  entry.hasAbbrTitle = false;
  entry.hasWikiText = false;
  entry.pendingLenAtOpen = self->pendingLen;
  entry.unknownTagName[0] = '\0';

  if (!parentSuppresses) {
    switch (action) {
      case TagAction::BLOCK_STRIP:
        self->flushPending();
        entry.suppressChildren = true;
        break;

      case TagAction::FORMAT_BOLD:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::FORMAT_ITALIC:
      case TagAction::VAR:
        self->flushPending();
        self->fmt.italic = true;
        break;

      case TagAction::FORMAT_UNDERLINE:
        self->flushPending();
        self->fmt.underline = true;
        break;

      case TagAction::FORMAT_STRIKE:
        self->flushPending();
        self->fmt.strikethrough = true;
        break;

      case TagAction::FORMAT_SMALL:
        self->flushPending();
        break;

      case TagAction::FORMAT_CODE:
        self->flushPending();
        self->fmt.bold = true;
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        self->newlinePending = true;
        self->listItemPending = true;
        self->fmt.indentLevel++;
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        self->fmt.bold = true;
        break;

      case TagAction::ABBR: {
        self->flushPending();  // Flush preceding text before body accumulates
        const char* title = findAttr(atts, "title");
        if (title) {
          entry.hasAbbrTitle = true;
          strncpy(entry.abbrTitle, title, sizeof(entry.abbrTitle) - 1);
          entry.abbrTitle[sizeof(entry.abbrTitle) - 1] = '\0';
        }
        break;
      }

      case TagAction::WIKI_ANNOT: {
        const char* colon = strchr(name, ':');
        if (colon && colon[1] != '\0') {
          entry.hasWikiText = true;
          strncpy(entry.wikiText, colon + 1, sizeof(entry.wikiText) - 1);
          entry.wikiText[sizeof(entry.wikiText) - 1] = '\0';
        }
        break;
      }

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        strncpy(entry.unknownTagName, name, sizeof(entry.unknownTagName) - 1);
        entry.unknownTagName[sizeof(entry.unknownTagName) - 1] = '\0';
        entry.pendingLenAtOpen = self->pendingLen;
#endif
        break;

      default:
        break;
    }
  }

  if (self->stackDepth < MAX_STACK) {
    self->stack[self->stackDepth++] = entry;
  }
}

void XMLCALL DictHtmlRenderer::onEnd(void* ud, const XML_Char* name) {
  (void)name;
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;
  if (self->stackDepth == 0) return;

  StackEntry& entry = self->stack[self->stackDepth - 1];
  bool suppressed = entry.suppressChildren || entry.action == TagAction::BLOCK_STRIP;

  if (!suppressed) {
    switch (entry.action) {
      case TagAction::FORMAT_BOLD:
      case TagAction::FORMAT_ITALIC:
      case TagAction::FORMAT_UNDERLINE:
      case TagAction::FORMAT_STRIKE:
      case TagAction::FORMAT_SMALL:
      case TagAction::FORMAT_CODE:
      case TagAction::VAR:
      case TagAction::SPAN:
      case TagAction::REGISTERED:
        self->flushPending();
        break;

      case TagAction::BLOCK_BREAK:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::BLOCK_QUOTE:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_CONTAINER:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::LIST_ITEM:
        self->flushPending();
        break;

      case TagAction::HEADING:
        self->flushPending();
        self->newlinePending = true;
        break;

      case TagAction::ABBR:
        if (entry.hasAbbrTitle) {
          self->flushPending();
          char buf[72];
          int n = snprintf(buf, sizeof(buf), " (%s)", entry.abbrTitle);
          if (n > 0 && n < static_cast<int>(sizeof(buf))) {
            self->emitText(buf, n);
          }
          self->flushPending();
        }
        break;

      case TagAction::WIKI_ANNOT:
        if (entry.hasWikiText) {
          self->flushPending();  // Flush preceding text before emitting wiki suffix
          self->emitText(entry.wikiText, static_cast<int>(strlen(entry.wikiText)));
          self->flushPending();
        }
        break;

      case TagAction::STRIP_KEEP:
#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN
        if (entry.unknownTagName[0] != '\0') {
          // Extract word before (from text accumulated before tag opened)
          char wordBefore[32] = {};
          // We can't easily recover the text before — use empty for now
          // Extract tag contents: text accumulated between open and close
          char tagContents[64] = {};
          int contStart = entry.pendingLenAtOpen;
          int contLen = self->pendingLen - contStart;
          if (contLen > 0 && contLen < static_cast<int>(sizeof(tagContents))) {
            memcpy(tagContents, self->pendingText + contStart, contLen);
            tagContents[contLen] = '\0';
          }
          self->recordUnknownTag(entry.unknownTagName, wordBefore, tagContents, "");
        }
#endif
        self->flushPending();
        break;

      default:
        break;
    }

    // Restore saved format (bold/italic/underline/strikethrough/indentLevel)
    self->fmt = entry.savedFmt;
  }

  self->stackDepth--;
}

void XMLCALL DictHtmlRenderer::onText(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<DictHtmlRenderer*>(ud);
  if (self->parseError) return;

  if (self->stackDepth > 0) {
    const StackEntry& top = self->stack[self->stackDepth - 1];
    // Suppress text inside block-strip and for wiki-annot (emit from tag name, not body)
    if (top.suppressChildren || top.action == TagAction::WIKI_ANNOT || top.action == TagAction::BLOCK_STRIP) {
      return;
    }
  }

  self->emitText(s, len);
}

// ---------------------------------------------------------------------------
// Public render()
// ---------------------------------------------------------------------------

const std::vector<StyledSpan>& DictHtmlRenderer::render(const char* html, int len) {
  reset();

  if (!parser) {
    parseError = true;
    return spans;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onText);

  // Wrap fragment in a synthetic root element so expat sees well-formed XML
  static const char kOpen[] = "<_root>";
  static const char kClose[] = "</_root>";

  if (XML_Parse(parser, kOpen, static_cast<int>(sizeof(kOpen) - 1), 0) != XML_STATUS_OK) {
    parseError = true;
    return spans;
  }
  if (XML_Parse(parser, html, len, 0) != XML_STATUS_OK) {
    parseError = true;
    return spans;
  }
  if (XML_Parse(parser, kClose, static_cast<int>(sizeof(kClose) - 1), 1) != XML_STATUS_OK) {
    parseError = true;
    return spans;
  }

  flushPending();
  return spans;
}

// ---------------------------------------------------------------------------
// Unknown tag tracking (smoke test only)
// ---------------------------------------------------------------------------

#ifdef DICT_HTML_RENDERER_TRACK_UNKNOWN

void DictHtmlRenderer::recordUnknownTag(const char* tagName, const char* wordBefore, const char* tagContents,
                                        const char* wordAfter) {
  if (unknownTagCount >= MAX_UNKNOWN_TAGS) return;
  UnknownTagInfo& info = unknownTags[unknownTagCount++];
  strncpy(info.tag, tagName, sizeof(info.tag) - 1);
  info.tag[sizeof(info.tag) - 1] = '\0';
  strncpy(info.entry, currentEntryName ? currentEntryName : "", sizeof(info.entry) - 1);
  info.entry[sizeof(info.entry) - 1] = '\0';
  strncpy(info.wordBefore, wordBefore ? wordBefore : "", sizeof(info.wordBefore) - 1);
  info.wordBefore[sizeof(info.wordBefore) - 1] = '\0';
  strncpy(info.tagContents, tagContents ? tagContents : "", sizeof(info.tagContents) - 1);
  info.tagContents[sizeof(info.tagContents) - 1] = '\0';
  strncpy(info.wordAfter, wordAfter ? wordAfter : "", sizeof(info.wordAfter) - 1);
  info.wordAfter[sizeof(info.wordAfter) - 1] = '\0';
}

void DictHtmlRenderer::extractLastWord(const char* text, int len, char* out, int outSize) {
  if (!text || len <= 0 || !out || outSize <= 0) {
    if (out && outSize > 0) out[0] = '\0';
    return;
  }
  int end = len - 1;
  while (end >= 0 && (text[end] == ' ' || text[end] == '\t' || text[end] == '\n')) end--;
  if (end < 0) {
    out[0] = '\0';
    return;
  }
  int start = end;
  while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '\t' && text[start - 1] != '\n') start--;
  int wlen = end - start + 1;
  if (wlen >= outSize) wlen = outSize - 1;
  memcpy(out, text + start, wlen);
  out[wlen] = '\0';
}

#endif  // DICT_HTML_RENDERER_TRACK_UNKNOWN
