#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace clippingText {
inline bool hasVisibleText(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; ++p) {
    if (*p > ' ') return true;
  }
  return false;
}

inline bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<uint8_t>(text[0]) == 0xE2 && static_cast<uint8_t>(text[1]) == 0x80 &&
         static_cast<uint8_t>(text[2]) == 0x83;
}

// Rendered discretionary hyphens have no source codepoint; literal hyphens do.
inline bool append(std::string& text, std::string_view word, const char separator, const size_t limit,
                   const bool discretionaryHyphen = false) {
  if (discretionaryHyphen && !word.empty() && word.back() == '-') {
    word.remove_suffix(1);
  }
  const bool addSeparator = !text.empty() && separator != '\0';
  if (text.size() + word.size() + addSeparator > limit) return false;
  if (addSeparator) text.push_back(separator);
  text.append(word);
  return true;
}
}  // namespace clippingText
