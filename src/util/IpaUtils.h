#pragma once
#include <Utf8.h>

#include <cstdint>
#include <string>
#include <vector>

/// Returns true if the Unicode codepoint falls within an IPA phonetic range.
/// Ranges covered:
///   U+0250–U+02AF  IPA Extensions
///   U+02B0–U+02FF  Modifier Letters (IPA subset)
///   U+1D00–U+1D7F  Phonetic Extensions
///   U+1D80–U+1DBF  Phonetic Extensions Supplement
static inline bool isIpaCodepoint(uint32_t cp) {
  return (cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF);
}

struct IpaTextSpan {
  std::string text;
  bool isIpa;
};

/// Split a UTF-8 string into runs of IPA vs non-IPA codepoints.
static inline std::vector<IpaTextSpan> splitIpaRuns(const std::string& text) {
  std::vector<IpaTextSpan> result;
  if (text.empty()) return result;
  std::string current;
  bool currentIsIpa = false;
  bool first = true;
  const auto* p = reinterpret_cast<const uint8_t*>(text.c_str());
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const bool ipa = isIpaCodepoint(cp);
    if (!first && ipa != currentIsIpa) {
      result.push_back({std::move(current), currentIsIpa});
      current.clear();
    }
    currentIsIpa = ipa;
    first = false;
    // Re-encode cp to UTF-8
    if (cp < 0x80) {
      current += static_cast<char>(cp);
    } else if (cp < 0x800) {
      current += static_cast<char>(0xC0 | (cp >> 6));
      current += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      current += static_cast<char>(0xE0 | (cp >> 12));
      current += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      current += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      current += static_cast<char>(0xF0 | (cp >> 18));
      current += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      current += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      current += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  if (!current.empty()) result.push_back({std::move(current), currentIsIpa});
  return result;
}
