#include "SerialCommandGrammar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SerialCommandGrammar {

namespace {

bool needsQuotes(const std::string_view value) {
  return value.empty() || std::any_of(value.begin(), value.end(), [](const char c) {
           return c == ' ' || c == '"' || c == '\\' || c == '=' || static_cast<unsigned char>(c) < 0x20;
         });
}

bool isEscaped(const char c) { return c == '"' || c == '\\'; }

// The bytes appendField() writes for `value`, quotes and escapes included.
size_t valueSize(const std::string_view value) {
  const bool quoted = needsQuotes(value);
  size_t size = quoted ? 2 : 0;
  for (const char c : value) size += quoted && isEscaped(c) ? 2 : 1;
  return size;
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
    if (quoted && isEscaped(c)) put('\\');
    put(c);
  }
  if (quoted) put('"');
}

bool ReplyLine::appendFieldClipped(const char* key, const std::string_view value, const size_t reserve) {
  const size_t keySize = 2 + strlen(key);  // " <key>="
  const size_t left = capacity - length;
  const size_t room = left > reserve + keySize ? left - reserve - keySize : 0;
  if (valueSize(value) <= room) {
    appendField(key, value);
    return true;
  }
  // The longest start that fits, counted as if quoted, ending before a character's first byte.
  size_t used = 2;
  size_t end = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80) end = i;
    used += isEscaped(value[i]) ? 2 : 1;
    if (used > room) break;
  }
  appendField(key, value.substr(0, end));
  return false;
}

bool appendClippedTail(ReplyLine& line, TailField* fields, const size_t count) {
  size_t markerSize = strlen(" clipped=") + (count > 0 ? count - 1 : 0);  // with the commas
  for (size_t i = 0; i < count; ++i) markerSize += strlen(fields[i].key);
  for (size_t i = 0; i < count; ++i) {
    size_t reserve = markerSize;
    for (size_t j = i + 1; j < count; ++j) reserve += strlen(fields[j].key) + 4;  // ` key=""`
    if (!line.appendFieldClipped(fields[i].key, fields[i].value, reserve)) fields[i].cut = true;
  }
  const char* separator = " clipped=";
  for (size_t i = 0; i < count; ++i) {
    if (!fields[i].cut) continue;
    line.append(separator);
    line.append(fields[i].key);
    separator = ",";
  }
  return !line.truncated();
}

void ReplyLine::appendField(const char* key, const long value) {
  char number[24];
  const int n = snprintf(number, sizeof(number), "%ld", value);
  appendField(key, std::string_view(number, n > 0 ? static_cast<size_t>(n) : 0));
}

}  // namespace SerialCommandGrammar
