#pragma once

#include <string_view>

// ASCII whitespace as defined by WHATWG (space, tab, LF, CR, FF), which is
// also the CSS whitespace set.
constexpr bool isAsciiWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Returned view aliases s; it is not null-terminated unless s ends at a terminator.
constexpr std::string_view trimAsciiWhitespace(std::string_view s) {
  while (!s.empty() && isAsciiWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isAsciiWhitespace(s.back())) s.remove_suffix(1);
  return s;
}
