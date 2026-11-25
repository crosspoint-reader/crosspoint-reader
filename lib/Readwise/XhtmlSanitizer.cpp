#include "XhtmlSanitizer.h"

#include <cstring>

#include "Epub/htmlEntities.h"

namespace readwise {
namespace {

// Elements re-emitted (without attributes). Index order is irrelevant; the
// stack stores indices into this table.
constexpr const char* kKeep[] = {
    "p",          "div",     "br",     "hr",         "h1",      "h2",      "h3",      "h4",     "h5",    "h6",
    "blockquote", "pre",     "code",   "em",         "i",       "strong",  "b",       "u",      "s",     "del",
    "ins",        "sub",     "sup",    "small",      "mark",    "q",       "cite",    "abbr",   "ul",    "ol",
    "li",         "dl",      "dt",     "dd",         "table",   "thead",   "tbody",   "tfoot",  "tr",    "td",
    "th",         "caption", "figure", "figcaption", "section", "article", "header",  "footer", "aside", "main",
    "span",       "kbd",     "samp",   "var",        "time",    "details", "summary",
};
constexpr size_t kKeepCount = sizeof(kKeep) / sizeof(kKeep[0]);

// Elements dropped together with everything inside them.
constexpr const char* kSkip[] = {
    "script", "style", "svg",   "math",   "noscript", "iframe", "template", "head",   "title",
    "object", "video", "audio", "canvas", "picture",  "select", "textarea", "button", "nav",
};
constexpr size_t kSkipCount = sizeof(kSkip) / sizeof(kSkip[0]);

int findIn(const char* const* table, size_t count, const char* name) {
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(table[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

// Skipped elements whose content is raw text (no markup inside).
bool isRawText(const char* name) {
  return std::strcmp(name, "script") == 0 || std::strcmp(name, "style") == 0 || std::strcmp(name, "textarea") == 0 ||
         std::strcmp(name, "title") == 0;
}

bool isVoidKept(const char* name) { return std::strcmp(name, "br") == 0 || std::strcmp(name, "hr") == 0; }

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isNameChar(char c) { return isAlpha(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' || c == '_'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

XhtmlSanitizer::XhtmlSanitizer(Sink sink) : sink_(std::move(sink)) {}

bool XhtmlSanitizer::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && ok_; ++i) step(data[i]);
  return ok_;
}

bool XhtmlSanitizer::finish() {
  if (state_ == State::Entity) {
    flushEntity(false);
  } else if (state_ == State::TagOpen && skipTag_ < 0) {
    out("&lt;");
  }
  if (need_) emitInvalid();
  while (depth_ > 0) {
    --depth_;
    out("</");
    out(kKeep[stack_[depth_]]);
    out(">");
  }
  state_ = State::Text;
  return flush() && ok_;
}

void XhtmlSanitizer::step(const char c) {
  switch (state_) {
    case State::Text:
      if (c == '<') {
        if (need_) emitInvalid();
        state_ = State::TagOpen;
      } else if (c == '&') {
        if (need_) emitInvalid();
        entityLen_ = 0;
        state_ = State::Entity;
      } else if (skipTag_ < 0) {
        textByte(static_cast<uint8_t>(c));
      }
      return;

    case State::RawText: {
      // Match "</" + skip tag name, case-insensitively; anything else is content.
      const char* tag = kSkip[skipTag_];
      const size_t tagLen = std::strlen(tag);
      if (rawMatch_ == 0) {
        if (c == '<') rawMatch_ = 1;
      } else if (rawMatch_ == 1) {
        rawMatch_ = (c == '/') ? 2 : (c == '<' ? 1 : 0);
      } else if (rawMatch_ < tagLen + 2) {
        if (lower(c) == tag[rawMatch_ - 2]) {
          ++rawMatch_;
        } else {
          rawMatch_ = (c == '<') ? 1 : 0;
        }
      } else if (isNameChar(c)) {
        rawMatch_ = 0;  // "</scripts": not our end tag
      } else {
        std::strcpy(name_, tag);
        nameLen_ = static_cast<uint8_t>(tagLen);
        nameOverflow_ = false;
        rawMatch_ = 0;
        state_ = State::EndTagRest;
        step(c);
      }
      return;
    }

    case State::Entity:
      if (c == ';') {
        flushEntity(true);
        state_ = State::Text;
      } else if ((isNameChar(c) || c == '#') && c != ':' && c != '-' && c != '_' && entityLen_ < kEntityMax - 2) {
        entity_[entityLen_++] = c;
      } else {
        flushEntity(false);
        state_ = State::Text;
        step(c);
      }
      return;

    case State::TagOpen:
      if (isAlpha(c)) {
        nameLen_ = 0;
        nameOverflow_ = false;
        quote_ = 0;
        lastNonSpace_ = 0;
        name_[nameLen_++] = lower(c);
        state_ = State::TagName;
      } else if (c == '/') {
        nameLen_ = 0;
        nameOverflow_ = false;
        state_ = State::EndTagName;
      } else if (c == '!') {
        bangLen_ = 0;
        state_ = State::Bang;
      } else if (c == '?') {
        state_ = State::Decl;
      } else {
        // A bare '<' in text.
        if (skipTag_ < 0) out("&lt;");
        state_ = State::Text;
        step(c);
      }
      return;

    case State::TagName:
      if (isNameChar(c)) {
        if (nameLen_ < kNameMax) {
          name_[nameLen_++] = lower(c);
        } else {
          nameOverflow_ = true;
        }
        return;
      }
      name_[nameLen_] = '\0';
      state_ = State::InTag;
      step(c);
      return;

    case State::InTag:
      if (quote_) {
        if (c == quote_) quote_ = 0;
        return;
      }
      if (c == '"' || c == '\'') {
        quote_ = c;
      } else if (c == '>') {
        onStartTag(lastNonSpace_ == '/');
        state_ = (skipTag_ >= 0 && isRawText(kSkip[skipTag_])) ? State::RawText : State::Text;
        rawMatch_ = 0;
        return;
      }
      if (!isSpace(c)) lastNonSpace_ = c;
      return;

    case State::EndTagName:
      if (isNameChar(c)) {
        if (nameLen_ < kNameMax) {
          name_[nameLen_++] = lower(c);
        } else {
          nameOverflow_ = true;
        }
        return;
      }
      name_[nameLen_] = '\0';
      if (c == '>') {
        onEndTag();
        state_ = State::Text;
      } else {
        state_ = State::EndTagRest;
      }
      return;

    case State::EndTagRest:
      if (c == '>') {
        onEndTag();
        state_ = State::Text;
      }
      return;

    case State::Bang:
      // "<!--" opens a comment; anything else is a declaration up to '>'.
      if (c == '-' && bangLen_ < 2) {
        if (++bangLen_ == 2) {
          dashRun_ = 0;
          state_ = State::Comment;
        }
        return;
      }
      state_ = State::Decl;
      step(c);
      return;

    case State::Comment:
      if (c == '-') {
        if (dashRun_ < 2) ++dashRun_;
      } else if (c == '>' && dashRun_ >= 2) {
        state_ = State::Text;
      } else {
        dashRun_ = 0;
      }
      return;

    case State::Decl:
      if (c == '>') state_ = State::Text;
      return;
  }
}

void XhtmlSanitizer::onStartTag(const bool selfClosing) {
  if (nameOverflow_) return;

  if (skipTag_ >= 0) {
    if (!selfClosing && std::strcmp(name_, kSkip[skipTag_]) == 0 && skipNest_ < 255) ++skipNest_;
    return;
  }

  const int skip = findIn(kSkip, kSkipCount, name_);
  if (skip >= 0) {
    if (!selfClosing) {
      skipTag_ = static_cast<int8_t>(skip);
      skipNest_ = 1;
    }
    return;
  }

  const int keep = findIn(kKeep, kKeepCount, name_);
  if (keep < 0) return;  // unknown or media: unwrap

  if (isVoidKept(name_)) {
    out("<");
    out(name_);
    out("/>");
    return;
  }
  if (selfClosing || depth_ >= kMaxDepth) return;

  stack_[depth_++] = static_cast<uint8_t>(keep);
  out("<");
  out(name_);
  out(">");
}

void XhtmlSanitizer::onEndTag() {
  if (nameOverflow_) return;

  if (skipTag_ >= 0) {
    if (std::strcmp(name_, kSkip[skipTag_]) == 0 && --skipNest_ == 0) skipTag_ = -1;
    return;
  }

  const int keep = findIn(kKeep, kKeepCount, name_);
  if (keep < 0) return;

  // Close up to the matching open element; ignore stray end tags.
  int match = -1;
  for (int i = depth_ - 1; i >= 0; --i) {
    if (stack_[i] == keep) {
      match = i;
      break;
    }
  }
  if (match < 0) return;
  while (depth_ > static_cast<uint8_t>(match)) {
    --depth_;
    out("</");
    out(kKeep[stack_[depth_]]);
    out(">");
  }
}

void XhtmlSanitizer::flushEntity(const bool terminatedBySemicolon) {
  if (skipTag_ >= 0) return;
  entity_[entityLen_] = '\0';

  if (terminatedBySemicolon && entityLen_ > 0) {
    if (entity_[0] == '#') {
      uint32_t cp = 0;
      bool valid = entityLen_ > 1;
      const bool hex = entityLen_ > 1 && (entity_[1] == 'x' || entity_[1] == 'X');
      for (size_t i = hex ? 2 : 1; i < entityLen_ && valid; ++i) {
        const char d = entity_[i];
        uint32_t v;
        if (d >= '0' && d <= '9') {
          v = static_cast<uint32_t>(d - '0');
        } else if (hex && lower(d) >= 'a' && lower(d) <= 'f') {
          v = static_cast<uint32_t>(lower(d) - 'a' + 10);
        } else {
          valid = false;
          break;
        }
        cp = cp * (hex ? 16 : 10) + v;
        if (cp > 0x10FFFF) valid = false;
      }
      if (hex && entityLen_ == 2) valid = false;
      if (valid) {
        emitCodepoint(cp);
        return;
      }
    } else {
      char full[kEntityMax + 3];
      full[0] = '&';
      std::memcpy(full + 1, entity_, entityLen_);
      full[entityLen_ + 1] = ';';
      full[entityLen_ + 2] = '\0';
      if (const char* utf8 = lookupHtmlEntity(full, entityLen_ + 2)) {
        // Re-escape the XML specials; everything else is plain UTF-8.
        if (std::strcmp(utf8, "&") == 0) {
          out("&amp;");
        } else if (std::strcmp(utf8, "<") == 0) {
          out("&lt;");
        } else if (std::strcmp(utf8, ">") == 0) {
          out("&gt;");
        } else {
          out(utf8);
        }
        ++textBytes_;
        return;
      }
    }
  }

  // Not an entity: the '&' was literal text.
  out("&amp;");
  for (size_t i = 0; i < entityLen_; ++i) textByte(static_cast<uint8_t>(entity_[i]));
  if (terminatedBySemicolon) textByte(';');
}

void XhtmlSanitizer::textByte(const uint8_t b) {
  if (need_ == 0) {
    if (b < 0x80) {
      if (b < 0x20 && b != '\t' && b != '\n' && b != '\r') return;  // not allowed in XML
      if (b == '<') {
        out("&lt;");
      } else if (b == '>') {
        out("&gt;");
      } else if (b == '&') {
        out("&amp;");
      } else {
        const char ch = static_cast<char>(b);
        out(&ch, 1);
      }
      if (!isSpace(static_cast<char>(b))) ++textBytes_;
      return;
    }
    if (b >= 0xC2 && b <= 0xDF) {
      need_ = 1;
      cp_ = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
      need_ = 2;
      cp_ = b & 0x0F;
    } else if (b >= 0xF0 && b <= 0xF4) {
      need_ = 3;
      cp_ = b & 0x07;
    } else {
      emitInvalid();
      return;
    }
    seqLen_ = static_cast<uint8_t>(need_ + 1);
    return;
  }

  if ((b & 0xC0) != 0x80) {
    emitInvalid();
    textByte(b);
    return;
  }
  cp_ = (cp_ << 6) | (b & 0x3F);
  if (--need_ == 0) {
    const bool overlong = (seqLen_ == 3 && cp_ < 0x800) || (seqLen_ == 4 && cp_ < 0x10000);
    if (overlong || cp_ > 0x10FFFF || (cp_ >= 0xD800 && cp_ <= 0xDFFF)) {
      emitInvalid();
    } else {
      emitCodepoint(cp_);
    }
  }
}

void XhtmlSanitizer::emitInvalid() {
  need_ = 0;
  emitCodepoint(0xFFFD);
}

void XhtmlSanitizer::emitCodepoint(uint32_t cp) {
  // Drop what XML 1.0 forbids: C0 controls (bar whitespace), surrogates,
  // and the U+FFFE/U+FFFF non-characters.
  if ((cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE ||
      cp == 0xFFFF || cp > 0x10FFFF) {
    return;
  }
  if (cp < 0x80) {
    textByte(static_cast<uint8_t>(cp));
    return;
  }
  char u[4];
  size_t n;
  if (cp < 0x800) {
    u[0] = static_cast<char>(0xC0 | (cp >> 6));
    u[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    u[0] = static_cast<char>(0xE0 | (cp >> 12));
    u[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    u[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    u[0] = static_cast<char>(0xF0 | (cp >> 18));
    u[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    u[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    u[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  out(u, n);
  // U+00A0 and the U+2000 spaces count as whitespace, not text.
  if (cp != 0xA0 && !(cp >= 0x2000 && cp <= 0x200B)) ++textBytes_;
}

void XhtmlSanitizer::out(const char* s) { out(s, std::strlen(s)); }

void XhtmlSanitizer::out(const char* s, size_t n) {
  while (n > 0 && ok_) {
    if (bufLen_ == kOutBuf && !flush()) return;
    const size_t take = (kOutBuf - bufLen_ < n) ? kOutBuf - bufLen_ : n;
    std::memcpy(buf_ + bufLen_, s, take);
    bufLen_ += take;
    s += take;
    n -= take;
  }
}

bool XhtmlSanitizer::flush() {
  if (bufLen_ > 0 && ok_) {
    ok_ = sink_(buf_, bufLen_);
    bufLen_ = 0;
  }
  return ok_;
}

}  // namespace readwise
