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
  while (start < end && std::isalpha(static_cast<unsigned char>(input[start]))) start++;
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

void appendCodepoint(std::string& output, uint32_t codepoint) {
  if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return;
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

bool appendNumericEntity(std::string& output, const char* entity, size_t len) {
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
  appendCodepoint(output, value);
  return true;
}

void appendBreak(std::string& output, size_t count = 1) {
  while (!output.empty() && output.back() == ' ') output.pop_back();
  if (output.empty()) return;
  size_t existing = 0;
  while (existing < output.size() && output[output.size() - existing - 1] == '\n') existing++;
  while (existing++ < count) output.push_back('\n');
}

}  // namespace

std::string htmlToPlainText(const std::string& html) {
  std::string output;
  output.reserve(html.size());

  for (size_t i = 0; i < html.size();) {
    if (html[i] == '<' && isTagStart(html, i)) {
      const size_t close = html.find('>', i + 1);
      if (close == std::string::npos) {
        output.push_back(html[i++]);
        continue;
      }
      const TagBreak separator = tagBreak(html, i + 1, close);
      if (separator == TagBreak::Line)
        appendBreak(output);
      else if (separator == TagBreak::Paragraph)
        appendBreak(output, 2);
      i = close + 1;
      continue;
    }

    if (html[i] == '&') {
      const size_t semicolon = html.find(';', i + 1);
      if (semicolon != std::string::npos && semicolon - i <= 16) {
        const size_t len = semicolon - i + 1;
        const char* value = lookupHtmlEntity(html.data() + i, len);
        if (value != nullptr) {
          output.append(value);
          i = semicolon + 1;
          continue;
        }
        if (appendNumericEntity(output, html.data() + i, len)) {
          i = semicolon + 1;
          continue;
        }
      }
    }

    const unsigned char c = html[i++];
    if (c == '\r' || c == '\t') {
      if (!output.empty() && output.back() != ' ' && output.back() != '\n') output.push_back(' ');
    } else if (c == '\n') {
      appendBreak(output);
    } else {
      output.push_back(static_cast<char>(c));
    }
  }

  while (!output.empty() && (output.back() == ' ' || output.back() == '\n')) output.pop_back();
  return output;
}
