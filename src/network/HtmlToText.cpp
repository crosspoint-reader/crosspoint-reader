#include "HtmlToText.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

// ── Entity decoding ───────────────────────────────────────────────────────────

struct NamedEntity {
  const char* name;
  uint32_t cp;
};

static constexpr NamedEntity NAMED_ENTITIES[] = {
    {"amp", '&'},    {"lt", '<'},      {"gt", '>'},      {"quot", '"'},   {"apos", '\''},
    {"nbsp", 0xA0},  {"copy", 0xA9},   {"reg", 0xAE},    {"trade", 0x2122}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"hellip", 0x2026}, {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0},  {"times", 0xD7},
    {"divide", 0xF7}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"pound", 0xA3},
    {"euro", 0x20AC}, {"yen", 0xA5},  {"cent", 0xA2},   {"sect", 0xA7},   {"para", 0xB6},
    {nullptr, 0},
};

uint32_t decodeEntity(const char* buf, size_t len) {
  if (len == 0) return 0;
  if (buf[0] == '#') {
    // Numeric entity
    char tmp[12] = {};
    size_t copyLen = len - 1;
    if (copyLen >= sizeof(tmp)) copyLen = sizeof(tmp) - 1;
    if (len > 1 && (buf[1] == 'x' || buf[1] == 'X')) {
      // Hex: &#x1F;
      size_t hexLen = len - 2;
      if (hexLen >= sizeof(tmp)) hexLen = sizeof(tmp) - 1;
      memcpy(tmp, buf + 2, hexLen);
      return static_cast<uint32_t>(strtoul(tmp, nullptr, 16));
    }
    memcpy(tmp, buf + 1, copyLen);
    return static_cast<uint32_t>(strtoul(tmp, nullptr, 10));
  }
  for (int i = 0; NAMED_ENTITIES[i].name; i++) {
    const char* n = NAMED_ENTITIES[i].name;
    size_t nlen = strlen(n);
    if (nlen == len && strncasecmp(buf, n, nlen) == 0) {
      return NAMED_ENTITIES[i].cp;
    }
  }
  return 0;
}

void appendUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// ── Tag classification ────────────────────────────────────────────────────────

// Case-insensitive prefix match where the tag name must end with a non-alnum char or EOS.
bool tagIs(const char* name, size_t nameLen, const char* tag) {
  size_t tlen = strlen(tag);
  if (nameLen != tlen) return false;
  return strncasecmp(name, tag, tlen) == 0;
}

bool isBlockTag(const char* name, size_t len) {
  static const char* const BLOCK[] = {
      "p",    "div",     "br",          "h1",   "h2",       "h3",        "h4",       "h5",
      "h6",   "li",      "ul",          "ol",   "tr",       "td",        "th",       "hr",
      "pre",  "blockquote", "article",  "section", "header", "footer",   "nav",      "main",
      "aside","figure",  "figcaption",  "dl",   "dt",       "dd",        "address",  "form",
      nullptr,
  };
  for (int i = 0; BLOCK[i]; i++) {
    if (tagIs(name, len, BLOCK[i])) return true;
  }
  return false;
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────────────────

std::string HtmlToText::convert(const std::string& html, size_t maxBytes) {
  const size_t inputLen = (maxBytes > 0 && maxBytes < html.size()) ? maxBytes : html.size();
  const char* src = html.data();

  std::string out;
  out.reserve(inputLen / 3);

  enum State { TEXT, IN_TAG, IN_COMMENT, IN_ENTITY, SKIP_SCRIPT, SKIP_STYLE };
  State state = TEXT;

  // Tag name buffer (only first 31 chars needed for matching)
  char tagName[32] = {};
  size_t tagNameLen = 0;
  bool isClosingTag = false;
  bool inTagName = false;  // still collecting tag name chars

  // Entity buffer (longest named entity is ~8 chars; numeric up to ~8 digits)
  char entityBuf[16] = {};
  size_t entityLen = 0;

  // Blank-line suppression: collapse runs of newlines to at most 2
  int trailingNewlines = 0;

  auto flushNewline = [&]() {
    // Remove any trailing space before the newline
    if (!out.empty() && out.back() == ' ') out.pop_back();
    trailingNewlines++;
    if (trailingNewlines <= 2) out += '\n';
  };

  auto appendText = [&](char c) {
    trailingNewlines = 0;
    out += c;
  };

  auto appendSpace = [&]() {
    if (!out.empty() && out.back() != ' ' && out.back() != '\n') {
      trailingNewlines = 0;
      out += ' ';
    }
  };

  size_t i = 0;
  while (i < inputLen) {
    const char c = src[i];

    // ── SKIP_SCRIPT ──────────────────────────────────────────────────────────
    if (state == SKIP_SCRIPT) {
      if (c == '<' && i + 8 <= inputLen && strncasecmp(src + i, "</script", 8) == 0) {
        while (i < inputLen && src[i] != '>') i++;
        if (i < inputLen) i++;
        state = TEXT;
      } else {
        i++;
      }
      continue;
    }

    // ── SKIP_STYLE ───────────────────────────────────────────────────────────
    if (state == SKIP_STYLE) {
      if (c == '<' && i + 7 <= inputLen && strncasecmp(src + i, "</style", 7) == 0) {
        while (i < inputLen && src[i] != '>') i++;
        if (i < inputLen) i++;
        state = TEXT;
      } else {
        i++;
      }
      continue;
    }

    // ── IN_COMMENT ───────────────────────────────────────────────────────────
    if (state == IN_COMMENT) {
      if (c == '-' && i + 2 < inputLen && src[i + 1] == '-' && src[i + 2] == '>') {
        i += 3;
        state = TEXT;
      } else {
        i++;
      }
      continue;
    }

    // ── IN_ENTITY ────────────────────────────────────────────────────────────
    if (state == IN_ENTITY) {
      if (c == ';' || entityLen >= sizeof(entityBuf) - 1) {
        entityBuf[entityLen] = '\0';
        uint32_t cp = decodeEntity(entityBuf, entityLen);
        if (cp != 0) {
          if (cp == 0xA0) {
            // Non-breaking space → regular space
            appendSpace();
          } else if (cp == '\n') {
            flushNewline();
          } else if (cp >= 32) {
            trailingNewlines = 0;
            appendUtf8(cp, out);
          }
        }
        entityLen = 0;
        state = TEXT;
        if (c == ';') i++;
        // If ended without ';' (buffer overflow), re-process current char in TEXT
      } else {
        entityBuf[entityLen++] = c;
        entityBuf[entityLen] = '\0';
        i++;
      }
      continue;
    }

    // ── IN_TAG ───────────────────────────────────────────────────────────────
    if (state == IN_TAG) {
      if (c == '>') {
        tagName[tagNameLen] = '\0';

        if (!isClosingTag) {
          if (tagIs(tagName, tagNameLen, "script")) {
            state = SKIP_SCRIPT;
            i++;
            continue;
          }
          if (tagIs(tagName, tagNameLen, "style")) {
            state = SKIP_STYLE;
            i++;
            continue;
          }
          if (isBlockTag(tagName, tagNameLen)) {
            flushNewline();
          }
        } else {
          // Closing block tags also mark paragraph boundaries
          if (isBlockTag(tagName, tagNameLen)) {
            flushNewline();
          }
        }
        state = TEXT;
        i++;
      } else if (inTagName) {
        if (isalnum(c) || c == '-') {
          if (tagNameLen < sizeof(tagName) - 1) {
            tagName[tagNameLen++] = static_cast<char>(tolower(c));
          }
        } else {
          // Space or '=' or '/' — tag name collection is done
          inTagName = false;
        }
        i++;
      } else {
        // Skip attribute chars until '>'
        i++;
      }
      continue;
    }

    // ── TEXT ─────────────────────────────────────────────────────────────────
    if (c == '<') {
      tagNameLen = 0;
      tagName[0] = '\0';
      isClosingTag = false;
      inTagName = false;
      i++;

      if (i >= inputLen) break;

      if (src[i] == '/') {
        // Closing tag
        isClosingTag = true;
        inTagName = true;
        i++;
      } else if (src[i] == '!' && i + 2 < inputLen && src[i + 1] == '-' && src[i + 2] == '-') {
        // HTML comment
        state = IN_COMMENT;
        i += 3;
      } else if (src[i] == '!' || src[i] == '?') {
        // DOCTYPE / processing instruction — skip to '>'
        while (i < inputLen && src[i] != '>') i++;
        if (i < inputLen) i++;
      } else if (isalpha(src[i])) {
        // Opening tag
        inTagName = true;
        state = IN_TAG;
      } else {
        // Malformed — treat as literal '<'
        appendText('<');
      }
      if (i < inputLen && state == TEXT && inTagName) {
        // First char of closing tag name not yet consumed; enter IN_TAG
        state = IN_TAG;
      }
      continue;
    }

    if (c == '&') {
      entityLen = 0;
      entityBuf[0] = '\0';
      state = IN_ENTITY;
      i++;
      continue;
    }

    // Whitespace normalisation in HTML
    if (c == '\n' || c == '\r' || c == '\t') {
      appendSpace();
      i++;
      continue;
    }
    if (c == ' ') {
      appendSpace();
      i++;
      continue;
    }

    // Printable text character
    appendText(c);
    i++;
  }

  // Strip any trailing whitespace / newlines
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
    out.pop_back();
  }

  return out;
}

std::string HtmlToText::extractTitle(const std::string& html) {
  // Case-insensitive search for <title>
  const char* p = html.data();
  const char* end = p + html.size();

  while (p + 7 <= end) {
    if (strncasecmp(p, "<title>", 7) == 0) {
      const char* start = p + 7;
      // Find </title>
      const char* q = start;
      while (q + 8 <= end) {
        if (strncasecmp(q, "</title>", 8) == 0) {
          return HtmlToText::convert(std::string(start, q));
        }
        q++;
      }
      return {};
    }
    p++;
  }
  return {};
}
