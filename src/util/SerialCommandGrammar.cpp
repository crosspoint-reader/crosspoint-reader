#include "SerialCommandGrammar.h"

#include <algorithm>
#include <cstdio>

namespace SerialCommandGrammar {

namespace {

bool needsQuotes(const std::string_view value) {
  return value.empty() || std::any_of(value.begin(), value.end(), [](const char c) {
           return c == ' ' || c == '"' || c == '\\' || c == '=' || static_cast<unsigned char>(c) < 0x20;
         });
}

}  // namespace

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

void splitHead(const std::string_view text, std::string_view& head, std::string_view& rest) {
  const size_t colon = text.find(':');
  head = text.substr(0, colon);
  rest = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
}

bool parseInt(const std::string_view text, int& out) {
  size_t i = 0;
  bool negative = false;
  if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;
  // Checked before each step: value * 10 + digit could wrap back into range first. The negative
  // range reaches one further (-2147483648).
  const uint32_t maxValue = negative ? 0x80000000u : 0x7fffffffu;
  uint32_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint32_t>(c - '0');
    if (value > (maxValue - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = static_cast<int>(negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value));
  return true;
}

bool parseButtonArg(const std::string_view arg, std::string_view& name, PressKind& kind, int& holdMs,
                    const int maxHoldMs) {
  std::string_view modifier;
  splitHead(arg, name, modifier);
  kind = PressKind::Short;
  if (name.empty()) return false;
  if (arg.find(':') == std::string_view::npos) return true;
  if (modifier == "LONG") {
    kind = PressKind::Long;
    return true;
  }
  if (modifier == "DOUBLE") {
    kind = PressKind::Double;
    return true;
  }
  int ms = 0;
  if (!parseInt(modifier, ms) || modifier[0] == '-' || modifier[0] == '+' || ms < 1 || ms > maxHoldMs) return false;
  kind = PressKind::Hold;
  holdMs = ms;
  return true;
}

void ReplyLine::put(const char c) {
  if (length < capacity) {
    buffer[length++] = c;
  } else {
    cut = true;
  }
}

void ReplyLine::append(const std::string_view text) {
  for (const char c : text) put(c);
}

void ReplyLine::appendField(const char* key, const std::string_view value) {
  put(' ');
  append(key);
  put('=');
  const bool quoted = needsQuotes(value);
  if (quoted) put('"');
  for (const char c : value) {
    if (static_cast<unsigned char>(c) < 0x20) {
      put(' ');
      continue;
    }
    if (quoted && (c == '"' || c == '\\')) put('\\');
    put(c);
  }
  if (quoted) put('"');
}

void ReplyLine::appendField(const char* key, const long value) {
  char number[24];
  const int n = snprintf(number, sizeof(number), "%ld", value);
  appendField(key, std::string_view(number, n > 0 ? static_cast<size_t>(n) : 0));
}

}  // namespace SerialCommandGrammar
