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
/// Results are appended into `out`; caller must clear `out` before each call.
///
/// F-065 (future): Add getTextWidth(fontId, data, len, Style) and
/// drawText(fontId, x, y, data, len, Style) overloads to GfxRenderer so that
/// IpaTextSpan can become a non-owning view {const char* data, size_t len, bool isIpa},
/// eliminating all per-run heap allocations here regardless of script or codepoint length.
static inline void splitIpaRuns(const char* text, std::vector<IpaTextSpan>& out) {
  if (!text || !text[0]) return;
  std::string current;
  bool currentIsIpa = false;
  bool first = true;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const bool ipa = isIpaCodepoint(cp);
    if (!first && ipa != currentIsIpa) {
      out.push_back({std::move(current), currentIsIpa});
      current.clear();
    }
    currentIsIpa = ipa;
    first = false;
    utf8AppendCodepoint(current, cp);
  }
  if (!current.empty()) out.push_back({std::move(current), currentIsIpa});
}
