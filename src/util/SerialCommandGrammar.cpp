#include "SerialCommandGrammar.h"

#include <cstdio>

namespace SerialCommandGrammar {

namespace {

bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

bool needsQuotes(const std::string_view value) {
  if (value.empty()) return true;
  for (const char c : value) {
    if (c == ' ' || c == '"' || c == '\\' || c == '=' || static_cast<unsigned char>(c) < 0x20) return true;
  }
  return false;
}

}  // namespace

bool parseButtonArg(const std::string_view arg, std::string_view& name, PressKind& kind) {
  const size_t colon = arg.find(':');
  name = arg.substr(0, colon);
  kind = PressKind::Short;
  if (name.empty()) return false;
  if (colon == std::string_view::npos) return true;
  const std::string_view modifier = arg.substr(colon + 1);
  if (modifier == "LONG") {
    kind = PressKind::Long;
    return true;
  }
  if (modifier == "DOUBLE") {
    kind = PressKind::Double;
    return true;
  }
  return false;
}

bool parseReaderTarget(const std::string_view arg, ReaderTarget& out) {
  out = ReaderTarget{};
  out.path = arg;
  const size_t at = arg.rfind('@');
  if (at == std::string_view::npos) return !arg.empty();
  const std::string_view position = arg.substr(at + 1);

  if (!position.empty() && position.back() == '%') {
    int percent = 0;
    if (!parseInt(position.substr(0, position.size() - 1), percent)) return !arg.empty();
    out.path = arg.substr(0, at);
    out.kind = ReaderTarget::Kind::Percent;
    out.percent = percent;
    return !out.path.empty() && percent >= 0 && percent <= 100;
  }

  const size_t colon = position.find(':');
  int spine = 0;
  int page = 0;
  if (colon == std::string_view::npos || !parseInt(position.substr(0, colon), spine) ||
      !parseInt(position.substr(colon + 1), page)) {
    return !arg.empty();  // not a position: the '@' belongs to the file name
  }
  out.path = arg.substr(0, at);
  out.kind = ReaderTarget::Kind::SpinePage;
  out.spine = spine;
  out.page = page;
  return !out.path.empty() && spine >= 1 && page >= 1;
}

bool splitKeyValue(const std::string_view arg, std::string_view& key, std::string_view& value) {
  const size_t eq = arg.find('=');
  if (eq == std::string_view::npos || eq == 0) return false;
  key = arg.substr(0, eq);
  value = arg.substr(eq + 1);
  return true;
}

bool parseInt(const std::string_view text, int& out) {
  size_t i = 0;
  bool negative = false;
  if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;
  // Checked before the step, not after it: `long` is 32 bits on the device, so value * 10 + digit
  // could wrap back into range first ("9999999999" became 1410065407 and was accepted).
  constexpr uint32_t kMax = 0x7fffffffu;
  uint32_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (kMax - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = negative ? -static_cast<int>(value) : static_cast<int>(value);
  return true;
}

bool parseIntList(std::string_view text, int* out, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const size_t comma = text.find(',');
    const bool last = i + 1 == count;
    // The last number takes the rest; a comma left in it means too many numbers.
    if (last != (comma == std::string_view::npos)) return false;
    if (!parseInt(text.substr(0, comma), out[i])) return false;
    if (!last) text.remove_prefix(comma + 1);
  }
  return true;
}

bool parseLogLevel(const std::string_view text, uint8_t& level) {
  // Tag and long name per level, in level order: "ERR" or "ERROR" is 0.
  static constexpr const char* kNames[] = {"ERR", "ERROR", "INF", "INFO", "DBG", "DEBUG", "TRC", "TRACE"};
  for (uint8_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
    if (equalsIgnoreCase(text, kNames[i])) {
      level = i / 2;
      return true;
    }
  }
  return false;
}

void appendField(std::string& out, const char* key, const std::string_view value) {
  out += ' ';
  out += key;
  out += '=';
  appendValue(out, value);
}

void appendValue(std::string& out, const std::string_view value) {
  if (!needsQuotes(value)) {
    out.append(value.data(), value.size());
    return;
  }
  out += '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
}

void appendField(std::string& out, const char* key, const long value) {
  char digits[24];
  snprintf(digits, sizeof(digits), "%ld", value);
  appendField(out, key, std::string_view(digits));
}

}  // namespace SerialCommandGrammar
