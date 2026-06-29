#pragma once
#include <Utf8.h>

#include <cstdint>
#include <string>
#include <vector>

/// Returns true if the Unicode codepoint falls within an IPA phonetic range, or
/// is a fallback symbol / math operator/ Greek character for dictionary definitions.
/// Ranges covered:
///   U+0220–U+02FF  Latin Extended-B / IPA Extensions / Spacing Modifier Letters
///   U+1D00–U+1DBF  Phonetic Extensions / Supplement
///   U+0370–U+03FF  Greek and Coptic (Fallback for etymology / definitions)
///   U+1E00–U+1FFF  Latin Extended Additional / Greek Extended (Fallback)
/// Combining marks used in IPA are attached to the previous run by splitDictRuns().
static inline bool isDictCodepoint(uint32_t cp) {
  if (cp < 0x0220) return false;
  return (cp >= 0x0220 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF) || cp == 0x221A || cp == 0x2192 ||
         cp == 0x261E || (cp >= 0x2153 && cp <= 0x2154) || (cp >= 0x266D && cp <= 0x266F) ||
         (cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1E00 && cp <= 0x1FFF);
}

struct DictTextSpan {
  std::string text;
  bool isDictFont;
};

/// Split a UTF-8 string into runs of IPA vs non-IPA codepoints.
/// Results are appended into `out`; caller must clear `out` before each call.
static inline void splitDictRuns(const char* text, std::vector<DictTextSpan>& out) {
  if (!text || !text[0]) return;
  std::string current;
  bool currentIsDictFont = false;
  bool first = true;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const bool combining = utf8IsCombiningMark(cp);
    const bool ipa = combining ? currentIsDictFont : isDictCodepoint(cp);
    if (!first && !combining && ipa != currentIsDictFont) {
      out.push_back({std::move(current), currentIsDictFont});
      current.clear();
    }
    currentIsDictFont = ipa;
    first = false;
    utf8AppendCodepoint(current, cp);
  }
  if (!current.empty()) out.push_back({std::move(current), currentIsDictFont});
}
