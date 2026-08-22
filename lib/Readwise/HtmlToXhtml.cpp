#include "HtmlToXhtml.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr HtmlToXhtml::TagDef kTags[] = {
    {"p", HtmlToXhtml::Kind::PARA_BLOCK},
    {"h1", HtmlToXhtml::Kind::PARA_BLOCK}, {"h2", HtmlToXhtml::Kind::PARA_BLOCK}, {"h3", HtmlToXhtml::Kind::PARA_BLOCK},
    {"h4", HtmlToXhtml::Kind::PARA_BLOCK}, {"h5", HtmlToXhtml::Kind::PARA_BLOCK}, {"h6", HtmlToXhtml::Kind::PARA_BLOCK},
    {"ul", HtmlToXhtml::Kind::BLOCK},
    {"ol", HtmlToXhtml::Kind::BLOCK},
    {"li", HtmlToXhtml::Kind::BLOCK},
    {"blockquote", HtmlToXhtml::Kind::BLOCK},
    {"pre", HtmlToXhtml::Kind::BLOCK},
    {"figure", HtmlToXhtml::Kind::BLOCK},
    {"figcaption", HtmlToXhtml::Kind::BLOCK},
    {"table", HtmlToXhtml::Kind::BLOCK},
    {"thead", HtmlToXhtml::Kind::BLOCK},
    {"tbody", HtmlToXhtml::Kind::BLOCK},
    {"tr", HtmlToXhtml::Kind::BLOCK},
    {"td", HtmlToXhtml::Kind::BLOCK},
    {"th", HtmlToXhtml::Kind::BLOCK},
    {"caption", HtmlToXhtml::Kind::BLOCK},
    {"em", HtmlToXhtml::Kind::INLINE},     {"i", HtmlToXhtml::Kind::INLINE},
    {"strong", HtmlToXhtml::Kind::INLINE}, {"b", HtmlToXhtml::Kind::INLINE},
    {"u", HtmlToXhtml::Kind::INLINE},      {"s", HtmlToXhtml::Kind::INLINE},
    {"del", HtmlToXhtml::Kind::INLINE},    {"ins", HtmlToXhtml::Kind::INLINE},
    {"small", HtmlToXhtml::Kind::INLINE},  {"code", HtmlToXhtml::Kind::INLINE},
    {"sup", HtmlToXhtml::Kind::INLINE},    {"sub", HtmlToXhtml::Kind::INLINE},
    {"a", HtmlToXhtml::Kind::INLINE},
    {"br", HtmlToXhtml::Kind::VOID_EMIT},
    {"hr", HtmlToXhtml::Kind::VOID_EMIT},
    // Dropped entirely, no output.
    {"img", HtmlToXhtml::Kind::IGNORE},
    {"embed", HtmlToXhtml::Kind::IGNORE},
    {"input", HtmlToXhtml::Kind::IGNORE},
    // Consumed together with their content.
    {"script", HtmlToXhtml::Kind::RAW},
    {"style", HtmlToXhtml::Kind::RAW},
    {"iframe", HtmlToXhtml::Kind::DROP_CONTENT},
    {"svg", HtmlToXhtml::Kind::DROP_CONTENT},
    {"noscript", HtmlToXhtml::Kind::DROP_CONTENT},
    {"form", HtmlToXhtml::Kind::DROP_CONTENT},
    {"button", HtmlToXhtml::Kind::DROP_CONTENT},
    {"select", HtmlToXhtml::Kind::DROP_CONTENT},
    {"textarea", HtmlToXhtml::Kind::DROP_CONTENT},
    {"video", HtmlToXhtml::Kind::DROP_CONTENT},
    {"audio", HtmlToXhtml::Kind::DROP_CONTENT},
    {"object", HtmlToXhtml::Kind::DROP_CONTENT},
    {"canvas", HtmlToXhtml::Kind::DROP_CONTENT},
    {"title", HtmlToXhtml::Kind::DROP_CONTENT},
};

struct NamedEntity {
  const char* name;
  const char* utf8;
};

// Common named entities resolved to UTF-8. Unknown names are dropped rather
// than emitted raw, which would produce XML-invalid bare ampersands.
constexpr NamedEntity kEntities[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
    {"nbsp", "\xC2\xA0"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
    {"hellip", "\xE2\x80\xA6"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
    {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"laquo", "\xC2\xAB"},
    {"raquo", "\xC2\xBB"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
    {"trade", "\xE2\x84\xA2"}, {"deg", "\xC2\xB0"}, {"middot", "\xC2\xB7"},
    {"bull", "\xE2\x80\xA2"}, {"sect", "\xC2\xA7"}, {"para", "\xC2\xB6"},
    {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"},
    {"eacute", "\xC3\xA9"}, {"egrave", "\xC3\xA8"}, {"agrave", "\xC3\xA0"},
    {"ccedil", "\xC3\xA7"}, {"szlig", "\xC3\x9F"}, {"uuml", "\xC3\xBC"},
    {"ouml", "\xC3\xB6"}, {"auml", "\xC3\xA4"}, {"oelig", "\xC5\x93"},
    {"dagger", "\xE2\x80\xA0"}, {"euro", "\xE2\x82\xAC"},
};

char lower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool isNameChar(const char c) {
  const char l = lower(c);
  return (l >= 'a' && l <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-';
}

bool isEntityChar(const char c) {
  const char l = lower(c);
  return (l >= 'a' && l <= 'z') || (c >= '0' && c <= '9') || c == '#' || c == 'x';
}

size_t encodeUtf8(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}
}  // namespace

void HtmlToXhtml::reset() {
  state_ = State::TEXT;
  nameLen_ = 0;
  isClosingTag_ = false;
  selfClosing_ = false;
  attrNameLen_ = 0;
  hrefLen_ = 0;
  attrQuote_ = 0;
  sawHref_ = false;
  entityLen_ = 0;
  bangLen_ = 0;
  commentDashRun_ = 0;
  dropDepth_ = 0;
  stackSize_ = 0;
  paragraphIndex_ = SIZE_MAX;
  rawTag_ = nullptr;
  rawMatchPos_ = 0;
  error_ = false;
}

const HtmlToXhtml::TagDef* HtmlToXhtml::findTag(const char* name) const {
  for (const auto& t : kTags) {
    if (strcmp(t.name, name) == 0) return &t;
  }
  return nullptr;
}

bool HtmlToXhtml::out(const char* s, const size_t len) {
  if (error_ || len == 0) return !error_;
  if (sink_.write(sink_.ctx, s, len) != 0) error_ = true;
  return !error_;
}

bool HtmlToXhtml::out(const char* s) { return out(s, strlen(s)); }

bool HtmlToXhtml::outEscapedText(const char* s, const size_t len) {
  for (size_t i = 0; i < len && !error_; i++) {
    switch (s[i]) {
      case '&':
        out("&amp;");
        break;
      case '<':
        out("&lt;");
        break;
      case '>':
        out("&gt;");
        break;
      default:
        out(s + i, 1);
        break;
    }
  }
  return !error_;
}

void HtmlToXhtml::emitClose(const TagDef* def) {
  char buf[MAX_NAME + 4];
  snprintf(buf, sizeof(buf), "</%s>", def->name);
  out(buf);
}

void HtmlToXhtml::closeTopTag() {
  if (stackSize_ == 0) return;
  const TagDef* def = stack_[stackSize_ - 1].def;
  emitClose(def);
  if (stackSize_ - 1 == paragraphIndex_) paragraphIndex_ = SIZE_MAX;
  stackSize_--;
}

void HtmlToXhtml::closeUpTo(const size_t target) {
  while (stackSize_ > target && !error_) closeTopTag();
}

void HtmlToXhtml::ensureParagraph() {
  if (paragraphIndex_ != SIZE_MAX || dropDepth_ > 0 || stackSize_ >= MAX_STACK) return;
  static constexpr TagDef P_DEF = {"p", Kind::BLOCK};
  if (!out("<p>")) return;
  stack_[stackSize_].def = &P_DEF;
  paragraphIndex_ = stackSize_;
  stackSize_++;
}

void HtmlToXhtml::startOpenTag(const TagDef& def, const bool selfClosing) {
  switch (def.kind) {
    case Kind::UNWRAP:
    case Kind::IGNORE:
      return;
    case Kind::RAW:
      if (dropDepth_ == 0) {
        rawTag_ = &def;
        rawMatchPos_ = 0;
        state_ = State::RAW_TEXT;
      }
      return;
    case Kind::DROP_CONTENT:
      if (dropDepth_ < MAX_DROP_DEPTH) dropStack_[dropDepth_++] = &def;
      return;
    default:
      break;
  }

  char buf[MAX_NAME + 3];
  if (def.kind == Kind::VOID_EMIT || selfClosing) {
    ensureParagraph();
    snprintf(buf, sizeof(buf), "<%s/>", def.name);
    out(buf);
    return;
  }

  if (def.kind == Kind::INLINE) {
    // Inline content always lives inside the loose-text paragraph.
    ensureParagraph();
  } else if (strcmp(def.name, "li") == 0) {
    // A new list item closes the previous one, back into the enclosing list.
    bool closedIntoList = false;
    for (size_t i = stackSize_; i > 0; i--) {
      const char* n = stack_[i - 1].def->name;
      if (strcmp(n, "ul") == 0 || strcmp(n, "ol") == 0) {
        closeUpTo(i);
        closedIntoList = true;
        break;
      }
    }
    if (!closedIntoList && paragraphIndex_ != SIZE_MAX) closeUpTo(paragraphIndex_);
  } else if (paragraphIndex_ != SIZE_MAX) {
    // Any other block closes the open paragraph/heading first.
    closeUpTo(paragraphIndex_);
  }
  if (stackSize_ >= MAX_STACK) return;  // too deep: keep the text, drop the markup

  bool emittedHref = false;
  if (&def == findTag("a") && sawHref_ && hrefLen_ > 0) {
    // Only http(s)/relative targets survive; dangerous schemes are stripped.
    bool safe = true;
    static constexpr const char* BAD_SCHEMES[] = {"javascript:", "data:", "vbscript:"};
    for (const char* scheme : BAD_SCHEMES) {
      size_t i = 0;
      while (scheme[i] != '\0' && lower(href_[i]) == scheme[i]) i++;
      if (scheme[i] == '\0') safe = false;
    }
    if (safe) {
      snprintf(buf, sizeof(buf), "<%s href=\"", def.name);
      if (!out(buf)) return;
      if (!outEscapedText(href_, hrefLen_)) return;
      if (!out("\">")) return;
      emittedHref = true;
    }
  }
  if (!emittedHref) {
    snprintf(buf, sizeof(buf), "<%s>", def.name);
    if (!out(buf)) return;
  }
  stack_[stackSize_].def = &def;
  // Only paragraph-capable blocks (p/hN) wrap loose text; other blocks
  // (ul, blockquote, li, ...) contain further structure.
  if (def.kind == Kind::PARA_BLOCK) paragraphIndex_ = stackSize_;
  stackSize_++;
}

void HtmlToXhtml::completeOpenTag() {
  tagName_[nameLen_] = '\0';
  if (dropDepth_ > 0) {
    // Nested suppressible tags unwind via their own close tags.
    const TagDef* def = findTag(tagName_);
    if (def && (def->kind == Kind::DROP_CONTENT || def->kind == Kind::RAW)) {
      if (dropDepth_ < MAX_DROP_DEPTH) dropStack_[dropDepth_++] = def;
      if (def->kind == Kind::RAW) {
        rawTag_ = def;
        rawMatchPos_ = 0;
        state_ = State::RAW_TEXT;
      }
    }
    return;
  }
  const TagDef* def = findTag(tagName_);
  if (!def) return;  // unwrap: children pass through
  startOpenTag(*def, selfClosing_);
}

void HtmlToXhtml::processCloseTag() {
  tagName_[nameLen_] = '\0';
  if (dropDepth_ > 0) {
    if (strcmp(dropStack_[dropDepth_ - 1]->name, tagName_) == 0) dropDepth_--;
    return;
  }
  // Find the innermost matching open element; close everything above it.
  for (size_t i = stackSize_; i > 0; i--) {
    if (strcmp(stack_[i - 1].def->name, tagName_) == 0) {
      closeUpTo(i - 1);
      return;
    }
  }
  // Unmatched close tags are ignored (already balanced or unknown).
}

void HtmlToXhtml::handleTextChar(const char c) {
  if (dropDepth_ > 0) return;

  switch (c) {
    case '<':
      state_ = State::TAG_START;
      return;
    case '&':
      state_ = State::ENTITY;
      entityLen_ = 0;
      return;
    case '>':
      ensureParagraph();
      out("&gt;");
      return;
    default:
      break;
  }
  if (static_cast<uint8_t>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
    return;  // XML-invalid control character: strip
  }
  ensureParagraph();
  out(&c, 1);
}

void HtmlToXhtml::handleEntityChar(const char c) {
  if (c == ';') {
    handleEntityEnd(true);
    return;
  }
  if (!isEntityChar(c)) {
    flushPendingEntityAsText(c);
    return;
  }
  if (entityLen_ >= MAX_ENTITY - 1) {
    flushPendingEntityAsText('\0');
    if (state_ != State::TEXT) return;
    handleTextChar(c);
    return;
  }
  entityBuf_[entityLen_++] = c;
}

void HtmlToXhtml::handleEntityEnd(const bool terminated) {
  state_ = State::TEXT;
  if (!terminated) return;

  char resolved[4];
  size_t rlen = 0;

  if (entityBuf_[0] == '#') {
    uint32_t cp = 0;
    bool ok = entityLen_ >= 2;
    if (ok && (entityBuf_[1] == 'x' || entityBuf_[1] == 'X')) {
      for (size_t i = 2; i < entityLen_; i++) {
        const char h = lower(entityBuf_[i]);
        cp *= 16;
        if (h >= '0' && h <= '9') {
          cp += h - '0';
        } else if (h >= 'a' && h <= 'f') {
          cp += h - 'a' + 10;
        } else {
          ok = false;
        }
      }
    } else if (ok) {
      for (size_t i = 1; i < entityLen_; i++) {
        if (entityBuf_[i] >= '0' && entityBuf_[i] <= '9') {
          cp = cp * 10 + static_cast<uint32_t>(entityBuf_[i] - '0');
        } else {
          ok = false;
        }
      }
    }
    if (ok && cp >= 0x20 && cp <= 0x10FFFF) rlen = encodeUtf8(cp, resolved);
  } else {
    for (const auto& e : kEntities) {
      if (strlen(e.name) == entityLen_ && memcmp(entityBuf_, e.name, entityLen_) == 0) {
        rlen = strlen(e.utf8);
        memcpy(resolved, e.utf8, rlen);
        break;
      }
    }
  }

  if (rlen > 0) {
    ensureParagraph();
    out(resolved, rlen);
  }
  // Unknown entities are dropped silently rather than emitting invalid XML.
}

void HtmlToXhtml::flushPendingEntityAsText(const char terminator) {
  // Not a real entity ("AT&T style"): re-emit the '&' plus what followed.
  entityBuf_[entityLen_] = '\0';
  state_ = State::TEXT;
  ensureParagraph();
  out("&amp;");
  outEscapedText(entityBuf_, entityLen_);
  if (terminator == '<') {
    handleTextChar('<');
  } else if (terminator != '\0') {
    handleTextChar(terminator);
  }
}

void HtmlToXhtml::handleTagNameChar(const char c) {
  if (c == '>') {
    completeOpenTag();
    // RAW tags switch the parser into RAW_TEXT inside completeOpenTag.
    if (rawTag_ == nullptr) state_ = State::TEXT;
    return;
  }
  if (c == '/') {
    selfClosing_ = true;
    state_ = State::TAG_ATTRS;
    return;
  }
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
    attrNameLen_ = 0;
    state_ = State::TAG_ATTRS;
    return;
  }
  if (isNameChar(c) && nameLen_ < MAX_NAME) {
    tagName_[nameLen_++] = lower(c);
  }
}

void HtmlToXhtml::handleCloseNameChar(const char c) {
  if (c == '>') {
    processCloseTag();
    state_ = State::TEXT;
    return;
  }
  if (isNameChar(c) && nameLen_ < MAX_NAME) {
    tagName_[nameLen_++] = lower(c);
  }
}

void HtmlToXhtml::captureAttrValue(const char c) {
  // We only keep href on <a>; everything else is discarded.
  if (sawHref_ && hrefLen_ < MAX_HREF) {
    href_[hrefLen_++] = c;
  }
}

void HtmlToXhtml::handleAttrChar(const char c) {
  switch (state_) {
    case State::ATTR_NAME:
      if (c == '=') {
        attrName_[attrNameLen_] = '\0';
        sawHref_ = strcmp(attrName_, "href") == 0;
        hrefLen_ = 0;
        state_ = State::ATTR_AFTER_NAME;
      } else if (c == '>') {
        completeOpenTag();
        if (rawTag_ == nullptr) state_ = State::TEXT;
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        attrNameLen_ = 0;
        state_ = State::TAG_ATTRS;
      } else if (isNameChar(c) && attrNameLen_ < sizeof(attrName_) - 1) {
        attrName_[attrNameLen_++] = lower(c);
      }
      break;

    case State::ATTR_AFTER_NAME:
      // The character right after '=' (or after a bare attribute name).
      if (c == '"' || c == '\'') {
        attrQuote_ = c;
        state_ = State::ATTR_VALUE;
      } else if (c == '>') {
        completeOpenTag();
        if (rawTag_ == nullptr) state_ = State::TEXT;
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        attrNameLen_ = 0;
        state_ = State::TAG_ATTRS;
      } else {
        // Unquoted value starts immediately (e.g. href=x).
        attrQuote_ = 0;
        state_ = State::ATTR_VALUE;
        captureAttrValue(c);
      }
      break;

    case State::ATTR_VALUE:
      if (attrQuote_ != 0) {
        if (static_cast<char>(c) == attrQuote_) {
          state_ = State::TAG_ATTRS;
          attrNameLen_ = 0;
        } else {
          captureAttrValue(c);
        }
      } else if (c == '"' || c == '\'') {
        attrQuote_ = c;
      } else if (c == '>') {
        completeOpenTag();
        if (rawTag_ == nullptr) state_ = State::TEXT;
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        attrNameLen_ = 0;
        state_ = State::TAG_ATTRS;
      } else {
        captureAttrValue(c);
      }
      break;

    default:
      break;
  }
}

void HtmlToXhtml::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len && !error_; i++) {
    const char c = data[i];
    switch (state_) {
      case State::TEXT:
        handleTextChar(c);
        break;

      case State::ENTITY:
        handleEntityChar(c);
        break;

      case State::TAG_START:
        if (c == '!') {
          bangLen_ = 0;
          state_ = State::BANG;
        } else if (c == '?') {
          state_ = State::SKIP_DECL;
        } else if (c == '/') {
          nameLen_ = 0;
          isClosingTag_ = true;
          state_ = State::CLOSING_NAME;
        } else if (lower(c) >= 'a' && lower(c) <= 'z') {
          nameLen_ = 0;
          selfClosing_ = false;
          sawHref_ = false;
          hrefLen_ = 0;
          isClosingTag_ = false;
          tagName_[nameLen_++] = lower(c);
          state_ = State::TAG_NAME;
        } else {
          // Bogus markup like "<3": emit the '<' as text and re-process.
          ensureParagraph();
          out("&lt;");
          handleTextChar(c);
        }
        break;

      case State::BANG:
        bang_[bangLen_++] = c;
        if (bangLen_ == 2) {
          if (bang_[0] == '-' && bang_[1] == '-') {
            commentDashRun_ = 0;
            state_ = State::COMMENT;
          } else {
            state_ = State::SKIP_DECL;
          }
        }
        break;

      case State::COMMENT:
        if (c == '-' ) {
          commentDashRun_++;
        } else if (c == '>' && commentDashRun_ >= 2) {
          state_ = State::TEXT;
        } else {
          commentDashRun_ = 0;
        }
        break;

      case State::SKIP_DECL:
        if (c == '>') state_ = State::TEXT;
        break;

      case State::TAG_NAME:
        handleTagNameChar(c);
        break;

      case State::CLOSING_NAME:
        handleCloseNameChar(c);
        break;

      case State::TAG_ATTRS:
        if (c == '>') {
          completeOpenTag();
          if (rawTag_ == nullptr) state_ = State::TEXT;
        } else if (c == '/') {
          selfClosing_ = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          // skip
        } else {
          attrNameLen_ = 0;
          state_ = State::ATTR_NAME;
          handleAttrChar(c);
        }
        break;

      case State::ATTR_NAME:
      case State::ATTR_AFTER_NAME:
      case State::ATTR_VALUE:
        handleAttrChar(c);
        break;

      case State::RAW_TEXT: {
        // Scan case-insensitively for "</name" to leave script/style content.
        const char* name = rawTag_ != nullptr ? rawTag_->name : "";
        const size_t nameLen = strlen(name);
        if (rawMatchPos_ == 0) {
          if (c == '<') rawMatchPos_ = 1;
        } else if (rawMatchPos_ == 1) {
          rawMatchPos_ = (c == '/') ? 2 : (c == '<' ? 1u : 0u);
        } else if (lower(c) == lower(name[rawMatchPos_ - 2])) {
          rawMatchPos_++;
          if (rawMatchPos_ == nameLen + 2) {
            // Full "</name" matched; consume up to and including '>'.
            rawTag_ = nullptr;
            rawMatchPos_ = 0;
            state_ = State::SKIP_DECL;
          }
        } else {
          rawMatchPos_ = (c == '<') ? 1 : 0;
        }
        break;
      }
    }
  }
}

void HtmlToXhtml::finish() {
  if (state_ == State::ENTITY) {
    flushPendingEntityAsText('\0');
  }
  // Partial tags are discarded silently; close whatever we emitted.
  closeUpTo(0);
  paragraphIndex_ = SIZE_MAX;
  state_ = State::TEXT;
}
