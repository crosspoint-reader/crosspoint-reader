#include "HtmlToPlainText.h"

#include <Epub/htmlEntities.h>

#include <cctype>
#include <cstdint>
#include <cstring>

namespace {

bool isTagStart(const std::string& input, size_t pos) {
  if (pos + 1 >= input.size()) return false;
  const unsigned char next = input[pos + 1];
  return next == '/' || next == '!' || next == '?' || std::isalpha(next);
}

enum class TagBreak : uint8_t { None, Line, Paragraph };

TagBreak tagBreak(const std::string& input, size_t start, size_t end) {
  while (start < end && (input[start] == '/' || std::isspace(static_cast<unsigned char>(input[start])))) start++;
  const size_t nameStart = start;
  // Alphanumeric, not alphabetic: a name scan that stops at the first digit
  // reads "h1" as "h", so none of the heading names below could ever match and
  // a definition's headings ran into the text after them.
  while (start < end && std::isalnum(static_cast<unsigned char>(input[start]))) start++;
  const size_t len = start - nameStart;
  if (len == 0) return TagBreak::None;

  const auto equals = [&](const char* name) {
    const size_t nameLen = strlen(name);
    if (len != nameLen) return false;
    for (size_t i = 0; i < len; i++) {
      if (std::tolower(static_cast<unsigned char>(input[nameStart + i])) != name[i]) return false;
    }
    return true;
  };

  if (equals("p") || equals("h1") || equals("h2") || equals("h3") || equals("h4") || equals("h5") || equals("h6") ||
      equals("hr")) {
    return TagBreak::Paragraph;
  }
  if (equals("br") || equals("div") || equals("li") || equals("tr")) return TagBreak::Line;
  return TagBreak::None;
}

// UTF-8 encode into `out` (room for 4); returns the byte count, 0 for a code
// point that is dropped (NUL, out of range, or a surrogate).
size_t encodeCodepoint(char* out, uint32_t codepoint) {
  if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return 0;
  if (codepoint <= 0x7F) {
    out[0] = static_cast<char>(codepoint);
    return 1;
  }
  if (codepoint <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
    out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
  out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
  return 4;
}

// Decode "&#N;" / "&#xH;" into `out` (room for 4). False when it is not a
// numeric reference, in which case it stays as written.
bool decodeNumericEntity(const char* entity, size_t len, char* out, size_t* outLen) {
  if (len < 4 || entity[0] != '&' || entity[1] != '#' || entity[len - 1] != ';') return false;
  size_t pos = 2;
  uint32_t base = 10;
  if (entity[pos] == 'x' || entity[pos] == 'X') {
    base = 16;
    pos++;
  }
  if (pos == len - 1) return false;

  uint32_t value = 0;
  for (; pos < len - 1; pos++) {
    const unsigned char c = entity[pos];
    uint32_t digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;
    if (value > (0x10FFFF - digit) / base) return false;
    value = value * base + digit;
  }
  *outLen = encodeCodepoint(out, value);
  return true;
}

// Ends the text so far with `count` newlines, folding into any already there.
void appendBreak(std::string& text, size_t& written, size_t count = 1) {
  while (written > 0 && text[written - 1] == ' ') written--;
  if (written == 0) return;
  size_t existing = 0;
  while (existing < written && text[written - existing - 1] == '\n') existing++;
  while (existing++ < count) text[written++] = '\n';
}

}  // namespace

// The output is written over the input it is read from, so a definition of
// tens of KB never needs a second buffer that size (with -fno-exceptions, the
// allocation failing aborts). `written` -- the output length -- never passes
// the read position `i`, because no step emits more bytes than it consumes: a
// tag is at least three bytes ("<p>") for its at most two breaks, and a decoded
// reference is never longer than the reference (checked where it is decoded,
// not assumed of the entity table). Lookahead (find, tagBreak) therefore only
// ever reads input that has not been overwritten.
//
// Ported from crosspoint-reader PR #3988 (Uri Tauber), which compacts into the
// input buffer the same way; the length guard is extended here to the numeric
// references as well.
void htmlToPlainTextInPlace(std::string& text) {
  size_t written = 0;

  for (size_t i = 0; i < text.size();) {
    if (text[i] == '<' && isTagStart(text, i)) {
      const size_t close = text.find('>', i + 1);
      if (close == std::string::npos) {
        text[written++] = text[i++];
        continue;
      }
      const TagBreak separator = tagBreak(text, i + 1, close);
      if (separator == TagBreak::Line)
        appendBreak(text, written);
      else if (separator == TagBreak::Paragraph)
        appendBreak(text, written, 2);
      i = close + 1;
      continue;
    }

    if (text[i] == '&') {
      const size_t semicolon = text.find(';', i + 1);
      if (semicolon != std::string::npos && semicolon - i <= 16) {
        const size_t len = semicolon - i + 1;
        // Decode before writing: the bytes being decoded are where the output
        // lands. A value longer than its reference would overrun the read
        // position, so it is left as written instead.
        const char* value = lookupHtmlEntity(text.data() + i, len);
        char decoded[4];
        size_t valueLen = 0;
        if (value != nullptr) {
          valueLen = strlen(value);
        } else if (decodeNumericEntity(text.data() + i, len, decoded, &valueLen)) {
          value = decoded;
        }
        if (value != nullptr && valueLen <= len) {
          memcpy(&text[written], value, valueLen);
          written += valueLen;
          i = semicolon + 1;
          continue;
        }
      }
    }

    const unsigned char c = text[i++];
    if (c == '\r' || c == '\t') {
      if (written > 0 && text[written - 1] != ' ' && text[written - 1] != '\n') text[written++] = ' ';
    } else if (c == '\n') {
      appendBreak(text, written);
    } else {
      text[written++] = static_cast<char>(c);
    }
  }

  while (written > 0 && (text[written - 1] == ' ' || text[written - 1] == '\n')) written--;
  // Shrinking never reallocates; the markup's bytes stay as slack capacity
  // rather than paying for a second, exact-size copy.
  text.resize(written);
}
