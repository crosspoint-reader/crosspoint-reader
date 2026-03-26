#pragma once
#include <cstdint>

/// Returns true if the Unicode codepoint falls within an IPA phonetic range.
/// Ranges covered:
///   U+0250–U+02AF  IPA Extensions
///   U+02B0–U+02FF  Modifier Letters (IPA subset)
///   U+1D00–U+1D7F  Phonetic Extensions
///   U+1D80–U+1DBF  Phonetic Extensions Supplement
static inline bool isIpaCodepoint(uint32_t cp) {
  return (cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF);
}
