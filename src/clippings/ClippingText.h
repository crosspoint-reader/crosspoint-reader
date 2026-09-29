#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace clippingText {
// Rendered discretionary hyphens have no source codepoint; literal hyphens do.
inline bool append(std::string& text, std::string_view word, const uint32_t start, const uint32_t end,
                   const char separator, const size_t limit) {
  if (start != UINT32_MAX && end != UINT32_MAX && end >= start && !word.empty() && word.back() == '-') {
    uint32_t codepoints = 0;
    for (const unsigned char c : word) {
      if ((c & 0xc0) != 0x80) ++codepoints;
    }
    if (codepoints > end - start) word.remove_suffix(1);
  }
  const bool addSeparator = !text.empty() && separator != '\0';
  if (text.size() + word.size() + addSeparator > limit) return false;
  if (addSeparator) text.push_back(separator);
  text.append(word);
  return true;
}
}  // namespace clippingText
