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
/// Additional IPA characters outside those blocks:
///   U+00E6, U+00F0, U+00F8, U+0127, U+014B, U+0153, U+03B2, U+03B8, U+03C7
/// Combining marks used in IPA are attached to the previous run by splitIpaRuns().
static inline bool isIpaCodepoint(uint32_t cp) {
  return cp == 0x00E6 || cp == 0x00F0 || cp == 0x00F8 || cp == 0x0127 || cp == 0x014B || cp == 0x0153 || cp == 0x03B2 ||
         cp == 0x03B8 || cp == 0x03C7 || (cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF);
}

struct IpaTextSpan {
  std::string text;
  bool isIpa;
};

/// Split a UTF-8 string into runs of IPA vs non-IPA codepoints.
/// Results are appended into `out`; caller must clear `out` before each call.
static inline void splitIpaRuns(const char* text, std::vector<IpaTextSpan>& out) {
  if (!text || !text[0]) return;
  std::string current;
  bool currentIsIpa = false;
  bool first = true;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const bool combining = utf8IsCombiningMark(cp);
    const bool ipa = combining ? currentIsIpa : isIpaCodepoint(cp);
    if (!first && !combining && ipa != currentIsIpa) {
      out.push_back({std::move(current), currentIsIpa});
      current.clear();
    }
    currentIsIpa = ipa;
    first = false;
    utf8AppendCodepoint(current, cp);
  }
  if (!current.empty()) out.push_back({std::move(current), currentIsIpa});
}
