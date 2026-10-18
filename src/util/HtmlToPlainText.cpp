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
  // a definition's headings ran into the text that followed them.
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

void appendCodepoint(std::string& output, size_t& written, uint32_t codepoint) {
  if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return;
  if (codepoint <= 0x7F) {
    output[written++] = static_cast<char>(codepoint);
  } else if (codepoint <= 0x7FF) {
    output[written++] = static_cast<char>(0xC0 | (codepoint >> 6));
    output[written++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0xFFFF) {
    output[written++] = static_cast<char>(0xE0 | (codepoint >> 12));
    output[written++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    output[written++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    output[written++] = static_cast<char>(0xF0 | (codepoint >> 18));
    output[written++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    output[written++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    output[written++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
}

bool appendNumericEntity(std::string& output, size_t& written, const char* entity, size_t len) {
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
  appendCodepoint(output, written, value);
  return true;
}

void appendBreak(std::string& output, size_t& written, size_t count = 1) {
  while (written > 0 && output[written - 1] == ' ') written--;
  if (written == 0) return;
  size_t existing = 0;
  while (existing < written && output[written - existing - 1] == '\n') existing++;
  while (existing++ < count) output[written++] = '\n';
}

}  // namespace

std::string htmlToPlainText(std::string html) {
  // Compact into consumed input bytes; replacements never expand the source.
  size_t written = 0;

  for (size_t i = 0; i < html.size();) {
    if (html[i] == '<' && isTagStart(html, i)) {
      const size_t close = html.find('>', i + 1);
      if (close == std::string::npos) {
        html[written++] = html[i++];
        continue;
      }
      const TagBreak separator = tagBreak(html, i + 1, close);
      if (separator == TagBreak::Line)
        appendBreak(html, written);
      else if (separator == TagBreak::Paragraph)
        appendBreak(html, written, 2);
      i = close + 1;
      continue;
    }

    if (html[i] == '&') {
      const size_t semicolon = html.find(';', i + 1);
      if (semicolon != std::string::npos && semicolon - i <= 16) {
        const size_t len = semicolon - i + 1;
        const char* value = lookupHtmlEntity(html.data() + i, len);
        const size_t valueLen = value ? strlen(value) : 0;
        if (value != nullptr && valueLen <= len) {
          memcpy(html.data() + written, value, valueLen);
          written += valueLen;
          i = semicolon + 1;
          continue;
        }
        if (appendNumericEntity(html, written, html.data() + i, len)) {
          i = semicolon + 1;
          continue;
        }
      }
    }

    const unsigned char c = html[i++];
    if (c == '\r' || c == '\t') {
      if (written > 0 && html[written - 1] != ' ' && html[written - 1] != '\n') html[written++] = ' ';
    } else if (c == '\n') {
      appendBreak(html, written);
    } else {
      html[written++] = static_cast<char>(c);
    }
  }

  while (written > 0 && (html[written - 1] == ' ' || html[written - 1] == '\n')) written--;
  html.resize(written);
  return html;
}
